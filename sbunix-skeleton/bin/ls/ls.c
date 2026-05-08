#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <syscall.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

/* -----------------------------------------------------------------------
 * Flags parsed from argv
 * ---------------------------------------------------------------------- */
static int flag_l = 0;   /* -l long format */
static int flag_a = 0;   /* -a show hidden (dot-files) */
static int flag_h = 0;   /* -h human-readable sizes */

/* -----------------------------------------------------------------------
 * Human-readable size (used with -h)
 * ---------------------------------------------------------------------- */
static void fmt_size(long size, char *out, int outsz) {
    if (!flag_h || size < 1024) {
        snprintf(out, outsz, "%ld", size);
        return;
    }
    const char *units[] = { "K", "M", "G", "T" };
    long val = size;
    int  ui  = -1;
    while (val >= 1024 && ui < 3) {
        val /= 1024;
        ui++;
    }
    snprintf(out, outsz, "%ld%s", val, units[ui]);
}

/* -----------------------------------------------------------------------
 * Mode string like "drwxr-xr-x"
 * ---------------------------------------------------------------------- */
static void fmt_mode(unsigned int mode, char *out) {
    /* type */
    if      ((mode & 0xF000) == 0x4000) out[0] = 'd';
    else if ((mode & 0xF000) == 0xA000) out[0] = 'l';
    else if ((mode & 0xF000) == 0x2000) out[0] = 'c';
    else if ((mode & 0xF000) == 0x6000) out[0] = 'b';
    else if ((mode & 0xF000) == 0x1000) out[0] = 'p';
    else if ((mode & 0xF000) == 0xC000) out[0] = 's';
    else                                 out[0] = '-';
    /* owner */
    out[1] = (mode & 0400) ? 'r' : '-';
    out[2] = (mode & 0200) ? 'w' : '-';
    out[3] = (mode & 0100) ? 'x' : '-';
    /* group */
    out[4] = (mode & 040)  ? 'r' : '-';
    out[5] = (mode & 020)  ? 'w' : '-';
    out[6] = (mode & 010)  ? 'x' : '-';
    /* other */
    out[7] = (mode & 04)   ? 'r' : '-';
    out[8] = (mode & 02)   ? 'w' : '-';
    out[9] = (mode & 01)   ? 'x' : '-';
    out[10] = '\0';
}

static void print_long_path(const char *path, const char *name) {
    struct stat st;
    char modestr[12];
    char szbuf[16];
    char linkbuf[256];
    int linklen = -1;

    if (lstat(path, &st) < 0) {
        printf("??????????   0        ?        %s\n", name);
        return;
    }

    fmt_mode(st.st_mode, modestr);
    fmt_size(st.st_size, szbuf, sizeof(szbuf));

    if (S_ISLNK(st.st_mode)) {
        linklen = readlink(path, linkbuf, sizeof(linkbuf) - 1);
        if (linklen >= 0)
            linkbuf[linklen] = '\0';
    }

    if (linklen >= 0) {
        printf("%s %d %s %s -> %s\n",
               modestr, (int)st.st_nlink, szbuf, name, linkbuf);
    } else {
        printf("%s %d %s %s\n",
               modestr, (int)st.st_nlink, szbuf, name);
    }
}

/* -----------------------------------------------------------------------
 * Single-entry display
 * ---------------------------------------------------------------------- */
static void print_entry(const char *dirpath, const char *name) {
    if (!flag_l) {
        printf("%s\n", name);
        return;
    }

    /* build full path for stat */
    char full[512];
    if (dirpath[0] == '/' && dirpath[1] == '\0') {
        snprintf(full, sizeof(full), "/%s", name);
    } else {
        snprintf(full, sizeof(full), "%s/%s", dirpath, name);
    }

    print_long_path(full, name);
}

/* -----------------------------------------------------------------------
 * List one directory
 * ---------------------------------------------------------------------- */
static int ls_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        /* might be a regular file */
        if (flag_l) {
            struct stat st;
            const char *name = path;
            /* extract basename */
            const char *slash = strrchr(path, '/');
            if (slash) name = slash + 1;
            if (lstat(path, &st) == 0) {
                print_long_path(path, name);
                return 0;
            }
        }
        dprintf(2, "ls: cannot access '%s': %s\n", path, strerror(errno));
        return 1;
    }

    /* Collect entries so we can sort them (static to avoid stack overflow) */
#define MAX_ENTRIES 256
    static char names[MAX_ENTRIES][256];
    int  count = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < MAX_ENTRIES) {
        /* filter hidden unless -a */
        if (!flag_a && de->d_name[0] == '.')
            continue;
        strncpy(names[count], de->d_name, 255);
        names[count][255] = '\0';
        count++;
    }
    closedir(d);

    /* Simple insertion sort by name */
    for (int i = 1; i < count; i++) {
        char tmp[256];
        strcpy(tmp, names[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            strcpy(names[j + 1], names[j]);
            j--;
        }
        strcpy(names[j + 1], tmp);
    }

    for (int i = 0; i < count; i++)
        print_entry(path, names[i]);
    return 0;
}

/* -----------------------------------------------------------------------
 * Parse a flags token like "-la" or "-al" or "-l"
 * Returns 1 if the token was a valid flag group, 0 otherwise.
 * ---------------------------------------------------------------------- */
static int parse_flags(const char *s) {
    if (s[0] != '-' || s[1] == '\0')
        return 0;
    for (int i = 1; s[i]; i++) {
        switch (s[i]) {
        case 'l': flag_l = 1; break;
        case 'a': flag_a = 1; break;
        case 'h': flag_h = 1; break;
        default:
            dprintf(2, "ls: invalid option -- '%c'\n", s[i]);
            return 0;
        }
    }
    return 1;
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    /* Collect path arguments after processing flags */
    const char *paths[64];
    int npaths = 0;

    for (int i = 1; i < argc; i++) {
        if (!parse_flags(argv[i])) {
            if (npaths < 64)
                paths[npaths++] = argv[i];
        }
    }

    int ret = 0;
    if (npaths == 0) {
        ret = ls_dir(".");
    } else if (npaths == 1) {
        ret = ls_dir(paths[0]);
    } else {
        for (int i = 0; i < npaths; i++) {
            printf("%s:\n", paths[i]);
            if (ls_dir(paths[i]))
                ret = 1;
            if (i + 1 < npaths)
                printf("\n");
        }
    }
    return ret;
}
