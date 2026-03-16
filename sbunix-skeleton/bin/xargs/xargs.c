#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_ARGS 256
#define BUF_SIZE 4096

static int run_cmd(char *cmd, char **cmd_argv) {
    pid_t pid = fork();
    if (pid < 0) {
        dprintf(2, "xargs: fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        execvp(cmd, cmd_argv);
        dprintf(2, "xargs: %s: %s\n", cmd, strerror(errno));
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

int main(int argc, char *argv[]) {
    char *cmd = "echo";
    int cmd_argc = 0;
    char *cmd_prefix[MAX_ARGS];
    int max_args = 0; /* 0 means unlimited */
    char *replace = NULL;
    int i = 1;

    /* Parse options */
    while (i < argc) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_args = atoi(argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            replace = argv[i + 1];
            if (max_args == 0) max_args = 1;
            i += 2;
        } else if (argv[i][0] == '-') {
            dprintf(2, "xargs: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            break;
        }
    }

    /* Remaining argv is the command prefix */
    if (i < argc) {
        cmd = argv[i];
        while (i < argc && cmd_argc < MAX_ARGS - 1)
            cmd_prefix[cmd_argc++] = argv[i++];
    } else {
        cmd_prefix[cmd_argc++] = cmd;
    }

    /* Read words from stdin */
    char buf[BUF_SIZE];
    int bpos = 0;
    char *words[MAX_ARGS];
    int nwords = 0;
    int ret = 0;

    ssize_t n;
    while ((n = read(STDIN_FILENO, buf + bpos, sizeof(buf) - bpos - 1)) > 0) {
        bpos += n;
        buf[bpos] = '\0';

        /* Extract complete words */
        int rp = 0;
        while (rp < bpos) {
            /* Skip whitespace */
            while (rp < bpos && is_ws(buf[rp]))
                rp++;
            if (rp >= bpos) break;

            /* Find end of word */
            int wp = rp;
            while (wp < bpos && !is_ws(buf[wp]))
                wp++;

            /* If we hit the end of buffer and there might be more, keep partial */
            if (wp == bpos && n > 0) break;

            buf[wp] = '\0';
            words[nwords] = strdup(buf + rp);
            nwords++;
            rp = wp + 1;

            /* Flush if max_args reached */
            if (max_args > 0 && nwords >= max_args) {
                char *exec_argv[MAX_ARGS];
                int ei = 0;

                if (replace) {
                    for (int j = 0; j < cmd_argc && ei < MAX_ARGS - 1; j++) {
                        if (strstr(cmd_prefix[j], replace)) {
                            /* Replace occurrences */
                            char tmp[BUF_SIZE];
                            char *s = cmd_prefix[j];
                            char *f;
                            int tpos = 0;
                            while ((f = strstr(s, replace)) != NULL) {
                                int plen = f - s;
                                memcpy(tmp + tpos, s, plen);
                                tpos += plen;
                                int wlen = strlen(words[0]);
                                memcpy(tmp + tpos, words[0], wlen);
                                tpos += wlen;
                                s = f + strlen(replace);
                            }
                            strcpy(tmp + tpos, s);
                            exec_argv[ei++] = strdup(tmp);
                        } else {
                            exec_argv[ei++] = cmd_prefix[j];
                        }
                    }
                } else {
                    for (int j = 0; j < cmd_argc && ei < MAX_ARGS - 1; j++)
                        exec_argv[ei++] = cmd_prefix[j];
                    for (int j = 0; j < nwords && ei < MAX_ARGS - 1; j++)
                        exec_argv[ei++] = words[j];
                }
                exec_argv[ei] = NULL;

                if (run_cmd(cmd, exec_argv) != 0) ret = 1;

                for (int j = 0; j < nwords; j++)
                    free(words[j]);
                nwords = 0;
            }
        }

        /* Shift remaining data to front */
        if (rp > 0 && rp < bpos) {
            memmove(buf, buf + rp, bpos - rp);
            bpos -= rp;
        } else if (rp >= bpos) {
            bpos = 0;
        }
    }

    /* Process remaining buffered data */
    if (bpos > 0) {
        buf[bpos] = '\0';
        int rp = 0;
        while (rp < bpos) {
            while (rp < bpos && is_ws(buf[rp])) rp++;
            if (rp >= bpos) break;
            int wp = rp;
            while (wp < bpos && !is_ws(buf[wp])) wp++;
            buf[wp] = '\0';
            words[nwords] = strdup(buf + rp);
            nwords++;
            rp = wp + 1;
        }
    }

    /* Flush remaining words */
    if (nwords > 0) {
        if (max_args > 0) {
            /* Execute in batches of max_args */
            int wi = 0;
            while (wi < nwords) {
                char *exec_argv[MAX_ARGS];
                int ei = 0;
                int batch = nwords - wi;
                if (batch > max_args) batch = max_args;

                if (replace) {
                    for (int j = 0; j < cmd_argc && ei < MAX_ARGS - 1; j++) {
                        if (strstr(cmd_prefix[j], replace)) {
                            char tmp[BUF_SIZE];
                            char *s = cmd_prefix[j];
                            char *f;
                            int tpos = 0;
                            while ((f = strstr(s, replace)) != NULL) {
                                int plen = f - s;
                                memcpy(tmp + tpos, s, plen);
                                tpos += plen;
                                int wlen = strlen(words[wi]);
                                memcpy(tmp + tpos, words[wi], wlen);
                                tpos += wlen;
                                s = f + strlen(replace);
                            }
                            strcpy(tmp + tpos, s);
                            exec_argv[ei++] = strdup(tmp);
                        } else {
                            exec_argv[ei++] = cmd_prefix[j];
                        }
                    }
                    wi++;
                } else {
                    for (int j = 0; j < cmd_argc && ei < MAX_ARGS - 1; j++)
                        exec_argv[ei++] = cmd_prefix[j];
                    for (int j = 0; j < batch && ei < MAX_ARGS - 1; j++)
                        exec_argv[ei++] = words[wi + j];
                    wi += batch;
                }
                exec_argv[ei] = NULL;
                if (run_cmd(cmd, exec_argv) != 0) ret = 1;
            }
        } else {
            char *exec_argv[MAX_ARGS];
            int ei = 0;
            for (int j = 0; j < cmd_argc && ei < MAX_ARGS - 1; j++)
                exec_argv[ei++] = cmd_prefix[j];
            for (int j = 0; j < nwords && ei < MAX_ARGS - 1; j++)
                exec_argv[ei++] = words[j];
            exec_argv[ei] = NULL;
            if (run_cmd(cmd, exec_argv) != 0) ret = 1;
        }

        for (int j = 0; j < nwords; j++)
            free(words[j]);
    }

    return ret;
}
