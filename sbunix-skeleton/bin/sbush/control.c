#include "sbush.h"

/* -----------------------------------------------------------------------
 * Control flow state machine — accumulates lines for multi-line constructs
 * (if/elif/else/fi, while/until/do/done, for/in/do/done, case/esac)
 * and executes them when the closing keyword is reached.
 *
 * KEY DESIGN: Each frame has an 'evaluating' flag.  When set, the frame
 * is being executed — its body buffers are being read by cf_run_lines.
 * cf_handle_line treats an evaluating frame as transparent: new control
 * flow constructs push onto the NEXT cf_stack slot, so the evaluating
 * frame's data is never overwritten by inner constructs.
 * ---------------------------------------------------------------------- */

enum cf_type  { CF_IF, CF_WHILE, CF_UNTIL, CF_FOR, CF_CASE };
enum if_phase { IF_COND, IF_THEN, IF_ELSE };
enum lp_phase { LOOP_COND, LOOP_BODY };

struct cf_frame {
    enum cf_type type;
    int  evaluating;      /* 1 when frame is being executed */

    /* if-specific */
    enum if_phase if_phase;
    int  if_matched;      /* already found a true branch */
    char if_cond[CF_BODY_SIZE];
    int  if_cond_len;
    char if_body[CF_BODY_SIZE];
    int  if_body_len;

    /* loop-specific (while/until/for) */
    enum lp_phase lp_phase;
    char lp_cond[CF_BODY_SIZE];
    int  lp_cond_len;
    char lp_body[CF_BODY_SIZE];
    int  lp_body_len;

    /* for-specific */
    char for_var[64];
    char for_words[MAX_LINE];

    /* case-specific */
    char case_word[256];
    char case_body[CF_BODY_SIZE];
    int  case_body_len;

    /* nesting counters (to track inner constructs during accumulation) */
    int nested_if;
    int nested_loop;
    int nested_case;
};

static struct cf_frame cf_stack[CF_MAX_DEPTH];
static int cf_depth = 0;

int cf_active(void) { return cf_depth; }

void cf_reset(void) { cf_depth = 0; }

/* Append a NUL-terminated line to a body buffer (NUL-separated entries) */
static int cf_append(char *buf, int *len, int maxlen, const char *line) {
    int slen = strlen(line);
    if (*len + slen + 1 >= maxlen) {
        dprintf(2, "sbush: control flow body buffer overflow\n");
        return -1;
    }
    memcpy(buf + *len, line, slen);
    *len += slen;
    buf[*len] = '\0';
    (*len)++;  /* NUL separator */
    return 0;
}

/* Execute NUL-separated lines by calling execute_line for each.
 * Use static buffer to avoid stack overflow in nested constructs. */
static char cf_run_tmp[MAX_LINE];

static void cf_run_lines(char *buf, int len) {
    int pos = 0;
    while (pos < len) {
        int i = 0;
        while (pos < len && buf[pos] != '\0' && i < MAX_LINE - 1)
            cf_run_tmp[i++] = buf[pos++];
        cf_run_tmp[i] = '\0';
        if (pos < len) pos++; /* skip NUL separator */
        if (i > 0)
            execute_line(cf_run_tmp);
        if (cf_break_count > 0 || cf_continue_flag || func_return_flag)
            break;
    }
}

/* Extract the first whitespace-delimited word from a line */
static int extract_first_word(const char *line, char *word, int maxword) {
    while (*line == ' ' || *line == '\t') line++;
    int i = 0;
    while (*line && *line != ' ' && *line != '\t' && *line != ';' && i < maxword - 1)
        word[i++] = *line++;
    word[i] = '\0';
    return i;
}

/* Split line on unquoted semicolons, process each segment.
 * Returns 1 if the line was consumed by control flow, 0 if not. */
static int cf_process_segment(const char *seg);
static void cf_exec_case(struct cf_frame *f);

/* break/continue globals */
int cf_break_count = 0;
int cf_continue_flag = 0;

int cf_handle_line(char *line) {
    /* If no active non-evaluating control flow, check if this line starts one */
    if (cf_depth == 0 ||
        (cf_depth > 0 && cf_stack[cf_depth - 1].evaluating)) {
        char word[64];
        extract_first_word(line, word, sizeof(word));
        if (strcmp(word, "if") != 0 &&
            strcmp(word, "while") != 0 &&
            strcmp(word, "until") != 0 &&
            strcmp(word, "for") != 0 &&
            strcmp(word, "case") != 0)
            return 0; /* not a control flow line */
    }

    /* Split on unquoted semicolons (but not ;; for case).
     * Use depth-indexed static buffers to avoid stack overflow while
     * remaining safe for recursion through cf_run_lines → execute_line. */
    static char bufs[MAX_EXEC_DEPTH][MAX_LINE];
    static int handle_depth = 0;
    if (handle_depth >= MAX_EXEC_DEPTH) return 0;
    char *buf = bufs[handle_depth++];
    strncpy(buf, line, MAX_LINE - 1);
    buf[MAX_LINE - 1] = '\0';

    char *p = buf;
    int in_sq = 0, in_dq = 0;
    char *seg_start = p;

    while (*p) {
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
        if (*p == '"'  && !in_sq) { in_dq = !in_dq; p++; continue; }
        if (!in_sq && !in_dq && *p == ';') {
            if (*(p+1) == ';') {
                p += 2; /* skip ;; (case clause terminator) */
            } else {
                *p = '\0';
                cf_process_segment(seg_start);
                p++;
                while (*p == ' ' || *p == '\t') p++;
                seg_start = p;
            }
        } else {
            p++;
        }
    }
    if (*seg_start)
        cf_process_segment(seg_start);

    handle_depth--;
    return 1; /* consumed by control flow */
}

/* -----------------------------------------------------------------------
 * Process a single segment (between semicolons or the whole line)
 * ---------------------------------------------------------------------- */
static int cf_process_segment(const char *seg) {
    /* Strip leading whitespace */
    while (*seg == ' ' || *seg == '\t') seg++;
    if (*seg == '\0') return 0;

    char word[64];
    extract_first_word(seg, word, sizeof(word));

    /* ===== If inside a non-evaluating construct, handle accumulation ===== */
    if (cf_depth > 0 && !cf_stack[cf_depth - 1].evaluating) {
        struct cf_frame *f = &cf_stack[cf_depth - 1];

        /* Track nesting of inner constructs */
        if (strcmp(word, "if") == 0) {
            f->nested_if++;
        } else if (strcmp(word, "while") == 0 || strcmp(word, "until") == 0 ||
                   strcmp(word, "for") == 0) {
            f->nested_loop++;
        } else if (strcmp(word, "case") == 0) {
            f->nested_case++;
        }

        /* Check for closing keywords of INNER constructs */
        if (strcmp(word, "fi") == 0 && f->nested_if > 0) {
            f->nested_if--;
            goto accumulate;
        }
        if (strcmp(word, "done") == 0 && f->nested_loop > 0) {
            f->nested_loop--;
            goto accumulate;
        }
        if (strcmp(word, "esac") == 0 && f->nested_case > 0) {
            f->nested_case--;
            goto accumulate;
        }

        /* Handle keywords for the CURRENT (outermost active) construct */
        if (f->type == CF_IF) {
            if (strcmp(word, "then") == 0 && f->nested_if == 0) {
                f->if_phase = IF_THEN;
                /* rest after "then" on same line */
                const char *rest = seg + 4;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest)
                    cf_append(f->if_body, &f->if_body_len, CF_BODY_SIZE, rest);
                return 1;
            }
            if (strcmp(word, "elif") == 0 && f->nested_if == 0) {
                /* Evaluate previous cond/body pair */
                if (!f->if_matched) {
                    f->evaluating = 1;
                    in_condition = 1;
                    cf_run_lines(f->if_cond, f->if_cond_len);
                    in_condition = 0;
                    if (last_exit_status == 0) {
                        f->if_matched = 1;
                        cf_run_lines(f->if_body, f->if_body_len);
                    }
                    f->evaluating = 0;
                }
                /* Reset for new condition */
                f->if_phase = IF_COND;
                f->if_cond_len = 0;
                f->if_body_len = 0;
                const char *rest = seg + 4;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest)
                    cf_append(f->if_cond, &f->if_cond_len, CF_BODY_SIZE, rest);
                return 1;
            }
            if (strcmp(word, "else") == 0 && f->nested_if == 0) {
                /* Evaluate previous cond/body pair */
                if (!f->if_matched) {
                    f->evaluating = 1;
                    in_condition = 1;
                    cf_run_lines(f->if_cond, f->if_cond_len);
                    in_condition = 0;
                    if (last_exit_status == 0) {
                        f->if_matched = 1;
                        cf_run_lines(f->if_body, f->if_body_len);
                    }
                    f->evaluating = 0;
                }
                /* else: always true if no previous match */
                f->if_phase = IF_ELSE;
                f->if_cond_len = 0;
                f->if_body_len = 0;
                /* rest after "else" on same line goes to body */
                const char *rest = seg + 4;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest)
                    cf_append(f->if_body, &f->if_body_len, CF_BODY_SIZE, rest);
                return 1;
            }
            if (strcmp(word, "fi") == 0 && f->nested_if == 0) {
                if (f->if_phase == IF_ELSE) {
                    /* Run else body if no prior branch matched */
                    if (!f->if_matched) {
                        f->evaluating = 1;
                        cf_run_lines(f->if_body, f->if_body_len);
                        f->evaluating = 0;
                    }
                } else {
                    /* Evaluate condition and run body if true */
                    if (!f->if_matched) {
                        f->evaluating = 1;
                        in_condition = 1;
                        cf_run_lines(f->if_cond, f->if_cond_len);
                        in_condition = 0;
                        if (last_exit_status == 0) {
                            cf_run_lines(f->if_body, f->if_body_len);
                        }
                        f->evaluating = 0;
                    }
                }
                cf_depth--;
                return 1;
            }

            /* Accumulate into appropriate buffer */
            if (f->if_phase == IF_COND) {
                cf_append(f->if_cond, &f->if_cond_len, CF_BODY_SIZE, seg);
            } else {
                cf_append(f->if_body, &f->if_body_len, CF_BODY_SIZE, seg);
            }
            return 1;
        }

        if (f->type == CF_WHILE || f->type == CF_UNTIL) {
            if (strcmp(word, "do") == 0 && f->nested_loop == 0) {
                f->lp_phase = LOOP_BODY;
                const char *rest = seg + 2;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest)
                    cf_append(f->lp_body, &f->lp_body_len, CF_BODY_SIZE, rest);
                return 1;
            }
            if (strcmp(word, "done") == 0 && f->nested_loop == 0) {
                f->evaluating = 1;
                while (1) {
                    in_condition = 1;
                    cf_run_lines(f->lp_cond, f->lp_cond_len);
                    in_condition = 0;
                    int cond_true = (last_exit_status == 0);
                    if (f->type == CF_UNTIL) cond_true = !cond_true;
                    if (!cond_true) break;
                    cf_run_lines(f->lp_body, f->lp_body_len);
                    if (cf_break_count > 0) { cf_break_count--; break; }
                    if (cf_continue_flag) { cf_continue_flag = 0; continue; }
                }
                f->evaluating = 0;
                cf_depth--;
                return 1;
            }

            if (f->lp_phase == LOOP_COND)
                cf_append(f->lp_cond, &f->lp_cond_len, CF_BODY_SIZE, seg);
            else
                cf_append(f->lp_body, &f->lp_body_len, CF_BODY_SIZE, seg);
            return 1;
        }

        if (f->type == CF_FOR) {
            if (strcmp(word, "do") == 0 && f->nested_loop == 0) {
                f->lp_phase = LOOP_BODY;
                const char *rest = seg + 2;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest)
                    cf_append(f->lp_body, &f->lp_body_len, CF_BODY_SIZE, rest);
                return 1;
            }
            if (strcmp(word, "done") == 0 && f->nested_loop == 0) {
                f->evaluating = 1;

                char words_buf[MAX_LINE];
                strncpy(words_buf, f->for_words, MAX_LINE - 1);
                words_buf[MAX_LINE - 1] = '\0';

                /* Iterate over space-separated words */
                char *wp = words_buf;
                while (*wp) {
                    while (*wp == ' ' || *wp == '\t') wp++;
                    if (!*wp) break;

                    char oneword[256];
                    int wi = 0;
                    if (*wp == '"') {
                        wp++;
                        while (*wp && *wp != '"' && wi < 255)
                            oneword[wi++] = *wp++;
                        if (*wp == '"') wp++;
                    } else if (*wp == '\'') {
                        wp++;
                        while (*wp && *wp != '\'' && wi < 255)
                            oneword[wi++] = *wp++;
                        if (*wp == '\'') wp++;
                    } else {
                        while (*wp && *wp != ' ' && *wp != '\t' && wi < 255)
                            oneword[wi++] = *wp++;
                    }
                    oneword[wi] = '\0';
                    if (wi == 0) continue;

                    var_set(f->for_var, oneword);
                    cf_run_lines(f->lp_body, f->lp_body_len);
                    if (cf_break_count > 0) { cf_break_count--; break; }
                    if (cf_continue_flag) { cf_continue_flag = 0; continue; }
                }
                f->evaluating = 0;
                cf_depth--;
                return 1;
            }

            /* Accumulate body */
            if (f->lp_phase == LOOP_BODY)
                cf_append(f->lp_body, &f->lp_body_len, CF_BODY_SIZE, seg);
            return 1;
        }

        if (f->type == CF_CASE) {
            if (strcmp(word, "esac") == 0 && f->nested_case == 0) {
                f->evaluating = 1;
                cf_exec_case(f);
                f->evaluating = 0;
                cf_depth--;
                return 1;
            }
            cf_append(f->case_body, &f->case_body_len, CF_BODY_SIZE, seg);
            return 1;
        }

    accumulate:
        /* Generic accumulation for constructs */
        if (f->type == CF_IF) {
            if (f->if_phase == IF_COND)
                cf_append(f->if_cond, &f->if_cond_len, CF_BODY_SIZE, seg);
            else
                cf_append(f->if_body, &f->if_body_len, CF_BODY_SIZE, seg);
        } else if (f->type == CF_WHILE || f->type == CF_UNTIL) {
            if (f->lp_phase == LOOP_COND)
                cf_append(f->lp_cond, &f->lp_cond_len, CF_BODY_SIZE, seg);
            else
                cf_append(f->lp_body, &f->lp_body_len, CF_BODY_SIZE, seg);
        } else if (f->type == CF_FOR) {
            cf_append(f->lp_body, &f->lp_body_len, CF_BODY_SIZE, seg);
        } else if (f->type == CF_CASE) {
            cf_append(f->case_body, &f->case_body_len, CF_BODY_SIZE, seg);
        }
        return 1;
    }

    /* ===== Starting a new construct ===== */
    if (cf_depth >= CF_MAX_DEPTH) {
        dprintf(2, "sbush: control flow nesting too deep\n");
        return 0;
    }

    if (strcmp(word, "if") == 0) {
        struct cf_frame *f = &cf_stack[cf_depth++];
        memset(f, 0, sizeof(*f));
        f->type = CF_IF;
        f->if_phase = IF_COND;
        /* rest of line after "if " is the condition */
        const char *rest = seg + 2;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (*rest)
            cf_append(f->if_cond, &f->if_cond_len, CF_BODY_SIZE, rest);
        return 1;
    }

    if (strcmp(word, "while") == 0 || strcmp(word, "until") == 0) {
        struct cf_frame *f = &cf_stack[cf_depth++];
        memset(f, 0, sizeof(*f));
        f->type = (word[0] == 'w') ? CF_WHILE : CF_UNTIL;
        f->lp_phase = LOOP_COND;
        const char *rest = seg + strlen(word);
        while (*rest == ' ' || *rest == '\t') rest++;
        if (*rest)
            cf_append(f->lp_cond, &f->lp_cond_len, CF_BODY_SIZE, rest);
        return 1;
    }

    if (strcmp(word, "for") == 0) {
        struct cf_frame *f = &cf_stack[cf_depth++];
        memset(f, 0, sizeof(*f));
        f->type = CF_FOR;
        f->lp_phase = LOOP_COND; /* will transition to LOOP_BODY on "do" */

        /* Parse: for VAR in WORDS... or for VAR; do (positional params) */
        const char *rest = seg + 3;
        while (*rest == ' ' || *rest == '\t') rest++;

        /* extract variable name */
        int vi = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && *rest != ';' && vi < 63)
            f->for_var[vi++] = *rest++;
        f->for_var[vi] = '\0';

        /* Skip whitespace after variable name */
        while (*rest == ' ' || *rest == '\t') rest++;

        /* Check for "in" keyword */
        if (rest[0] == 'i' && rest[1] == 'n' &&
            (rest[2] == ' ' || rest[2] == '\t' || rest[2] == '\0')) {
            rest += 2;
            while (*rest == ' ' || *rest == '\t') rest++;
            strncpy(f->for_words, rest, MAX_LINE - 1);
            f->for_words[MAX_LINE - 1] = '\0';
        } else {
            /* No "in" clause: iterate over positional parameters ($1, $2, ...) */
            int wpos = 0;
            for (int pi = 1; pi <= positional_count && wpos < MAX_LINE - 2; pi++) {
                char *pv = positional_get(pi);
                if (pv) {
                    if (pi > 1 && wpos < MAX_LINE - 1)
                        f->for_words[wpos++] = ' ';
                    while (*pv && wpos < MAX_LINE - 1)
                        f->for_words[wpos++] = *pv++;
                }
            }
            f->for_words[wpos] = '\0';
            /* Skip past semicolon if present (for VAR; do) */
            if (*rest == ';') rest++;
            while (*rest == ' ' || *rest == '\t') rest++;
        }
        return 1;
    }

    if (strcmp(word, "case") == 0) {
        struct cf_frame *f = &cf_stack[cf_depth++];
        memset(f, 0, sizeof(*f));
        f->type = CF_CASE;

        /* Parse: case WORD in */
        const char *rest = seg + 4;
        while (*rest == ' ' || *rest == '\t') rest++;

        int wi = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && wi < 255)
            f->case_word[wi++] = *rest++;
        f->case_word[wi] = '\0';
        /* skip optional "in" keyword — handled on this or next line */
        return 1;
    }

    /* Not a control flow keyword and no active construct — shouldn't reach here */
    return 0;
}

/* -----------------------------------------------------------------------
 * case/esac execution
 * ---------------------------------------------------------------------- */
static void cf_exec_case(struct cf_frame *f) {
    int depth = (int)(f - cf_stack);
    /* Expand the case word */
    char expanded_word[256];
    strncpy(expanded_word, f->case_word, 255);
    expanded_word[255] = '\0';

    /* Walk the accumulated body looking for pattern) ... ;; */
    char *buf = f->case_body;
    int len = f->case_body_len;
    int pos = 0;
    int matched = 0;

    /* Depth-indexed body buffer to survive recursive case evaluations */
    static char case_body_buf[CF_MAX_DEPTH][CF_BODY_SIZE];
    /* Temporary line buffers — safe as static since they're refilled each use */
    static char case_line[MAX_LINE];
    static char case_bline[MAX_LINE];

    while (pos < len && !matched) {
        /* Get next line */
        int li = 0;
        while (pos < len && buf[pos] != '\0' && li < MAX_LINE - 1)
            case_line[li++] = buf[pos++];
        case_line[li] = '\0';
        if (pos < len) pos++; /* skip NUL */

        /* Skip "in" keyword if present */
        char *lp = case_line;
        while (*lp == ' ' || *lp == '\t') lp++;
        if (lp[0] == 'i' && lp[1] == 'n' &&
            (lp[2] == '\0' || lp[2] == ' ' || lp[2] == '\t'))
            continue;

        /* Look for pattern ending with ) */
        char *paren = strrchr(lp, ')');
        if (!paren) continue;

        *paren = '\0';
        char *pattern = lp;

        /* Check if pattern matches (supports | for alternatives) */
        int pat_matched = 0;
        char *pp = pattern;
        while (*pp && !pat_matched) {
            char one_pat[256];
            int pi = 0;
            while (*pp && *pp != '|' && pi < 255) {
                if (*pp != ' ' && *pp != '\t')
                    one_pat[pi++] = *pp;
                pp++;
            }
            one_pat[pi] = '\0';
            if (*pp == '|') pp++;

            if (pi > 0 && glob_match(one_pat, expanded_word))
                pat_matched = 1;
        }

        if (!pat_matched) continue;

        /* Matched — collect and execute body lines until ;; */
        matched = 1;
        char *body = case_body_buf[depth];
        int body_len = 0;

        while (pos < len) {
            int bi = 0;
            while (pos < len && buf[pos] != '\0' && bi < MAX_LINE - 1)
                case_bline[bi++] = buf[pos++];
            case_bline[bi] = '\0';
            if (pos < len) pos++;

            /* Check for ;; terminator */
            char *bp = case_bline;
            while (*bp == ' ' || *bp == '\t') bp++;
            if (strcmp(bp, ";;") == 0)
                break;
            /* Also check if line ends with ;; */
            int blen = strlen(case_bline);
            if (blen >= 2 && case_bline[blen-1] == ';' && case_bline[blen-2] == ';') {
                case_bline[blen-2] = '\0';
                if (case_bline[0])
                    cf_append(body, &body_len, CF_BODY_SIZE, case_bline);
                break;
            }
            cf_append(body, &body_len, CF_BODY_SIZE, case_bline);
        }

        cf_run_lines(body, body_len);
    }
}
