/*
 * grep.c — simple grep for SBUnix.
 *
 * Supports: fixed-string matching, -i (case-insensitive), -v (invert),
 * -n (line numbers), -c (count), -l (files with matches), -r (recursive),
 * -w (whole word), -q (quiet), multiple files, stdin.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

static int opt_ignore_case;
static int opt_invert;
static int opt_line_num;
static int opt_count;
static int opt_files_only;
static int opt_recursive;
static int opt_whole_word;
static int opt_quiet;
static int match_found;

static int tolower_c(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Case-insensitive strstr */
static char *strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower_c((unsigned char)*h) == tolower_c((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

static int is_word_boundary(int c) {
    return !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
           !(c >= '0' && c <= '9') && c != '_';
}

static int line_matches(const char *line, const char *pattern) {
    const char *found;
    if (opt_ignore_case)
        found = strcasestr(line, pattern);
    else
        found = strstr(line, pattern);

    if (!found) return 0;

    if (opt_whole_word) {
        int plen = strlen(pattern);
        while (found) {
            int before_ok = (found == line) || is_word_boundary((unsigned char)found[-1]);
            int after_ok = (found[plen] == '\0') || is_word_boundary((unsigned char)found[plen]);
            if (before_ok && after_ok) return 1;
            /* Search for next occurrence */
            if (opt_ignore_case)
                found = strcasestr(found + 1, pattern);
            else
                found = strstr(found + 1, pattern);
        }
        return 0;
    }

    return 1;
}

static int grep_fd(int fd, const char *filename, const char *pattern, int show_name) {
    char buf[4096];
    int buf_len = 0;
    int line_no = 0;
    int count = 0;
    int file_matched = 0;

    for (;;) {
        ssize_t n = read(fd, buf + buf_len, sizeof(buf) - buf_len - 1);
        if (n <= 0 && buf_len == 0) break;
        if (n > 0) buf_len += n;
        buf[buf_len] = '\0';

        /* Process complete lines */
        char *start = buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            line_no++;

            int m = line_matches(start, pattern);
            if (opt_invert) m = !m;

            if (m) {
                file_matched = 1;
                match_found = 1;
                count++;
                if (opt_quiet) return 1;
                if (opt_files_only) {
                    printf("%s\n", filename);
                    return 1;
                }
                if (!opt_count) {
                    if (show_name) printf("%s:", filename);
                    if (opt_line_num) printf("%d:", line_no);
                    printf("%s\n", start);
                }
            }

            start = nl + 1;
        }

        /* Move remaining partial line to start of buffer */
        int remain = buf_len - (start - buf);
        if (remain > 0)
            memmove(buf, start, remain);
        buf_len = remain;

        if (n <= 0) {
            /* Process final line without newline */
            if (buf_len > 0) {
                buf[buf_len] = '\0';
                line_no++;
                int m = line_matches(buf, pattern);
                if (opt_invert) m = !m;
                if (m) {
                    file_matched = 1;
                    match_found = 1;
                    count++;
                    if (opt_quiet) return 1;
                    if (opt_files_only) {
                        printf("%s\n", filename);
                        return 1;
                    }
                    if (!opt_count) {
                        if (show_name) printf("%s:", filename);
                        if (opt_line_num) printf("%d:", line_no);
                        printf("%s\n", buf);
                    }
                }
            }
            break;
        }
    }

    if (opt_count) {
        if (show_name) printf("%s:", filename);
        printf("%d\n", count);
    }

    return file_matched;
}

static int grep_file(const char *path, const char *pattern, int show_name);

static int grep_dir(const char *path, const char *pattern) {
    DIR *d = opendir(path);
    if (!d) {
        dprintf(2, "grep: %s: No such file or directory\n", path);
        return 0;
    }
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (grep_file(child, pattern, 1))
            found = 1;
    }
    closedir(d);
    return found;
}

static int grep_file(const char *path, const char *pattern, int show_name) {
    struct stat st;
    if (stat(path, &st) < 0) {
        dprintf(2, "grep: %s: No such file or directory\n", path);
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        if (opt_recursive)
            return grep_dir(path, pattern);
        dprintf(2, "grep: %s: Is a directory\n", path);
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(2, "grep: %s: No such file or directory\n", path);
        return 0;
    }
    int r = grep_fd(fd, path, pattern, show_name);
    close(fd);
    return r;
}

int main(int argc, char *argv[]) {
    int i = 1;

    /* Parse options */
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
                case 'i': opt_ignore_case = 1; break;
                case 'v': opt_invert = 1; break;
                case 'n': opt_line_num = 1; break;
                case 'c': opt_count = 1; break;
                case 'l': opt_files_only = 1; break;
                case 'r': case 'R': opt_recursive = 1; break;
                case 'w': opt_whole_word = 1; break;
                case 'q': opt_quiet = 1; break;
                default:
                    dprintf(2, "grep: unknown option -%c\n", *o);
                    return 2;
            }
        }
        i++;
    }

    if (i >= argc) {
        dprintf(2, "usage: grep [-ivnclrwq] PATTERN [FILE...]\n");
        return 2;
    }

    const char *pattern = argv[i++];
    int nfiles = argc - i;

    if (nfiles == 0) {
        /* Read from stdin */
        grep_fd(STDIN_FILENO, "(stdin)", pattern, 0);
    } else {
        int show_name = (nfiles > 1) || opt_recursive;
        for (int j = i; j < argc; j++)
            grep_file(argv[j], pattern, show_name);
    }

    if (opt_quiet)
        return match_found ? 0 : 1;

    return match_found ? 0 : 1;
}
