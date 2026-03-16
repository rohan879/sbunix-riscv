#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fnmatch.h>

static char *name_pat = NULL;
static int type_filter = 0; /* 0=any, 'f'=file, 'd'=dir */
static int maxdepth = -1;   /* -1 = unlimited */

static int match_entry(const char *path, struct stat *st) {
    if (type_filter) {
        if (type_filter == 'f' && !S_ISREG(st->st_mode)) return 0;
        if (type_filter == 'd' && !S_ISDIR(st->st_mode)) return 0;
    }

    if (name_pat) {
        /* Match against basename only */
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (fnmatch(name_pat, base, 0) != 0) return 0;
    }

    return 1;
}

static int walk(const char *path, int depth) {
    struct stat st;
    if (stat(path, &st) < 0)
        return 0;

    if (match_entry(path, &st))
        printf("%s\n", path);

    if (!S_ISDIR(st.st_mode))
        return 0;

    if (maxdepth >= 0 && depth >= maxdepth)
        return 0;

    DIR *d = opendir(path);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char child[1024];
        int plen = strlen(path);
        int nlen = strlen(ent->d_name);
        if (plen + 1 + nlen >= (int)sizeof(child))
            continue;

        strcpy(child, path);
        if (plen > 0 && path[plen - 1] != '/')
            child[plen++] = '/';
        strcpy(child + plen, ent->d_name);

        walk(child, depth + 1);
    }
    closedir(d);
    return 0;
}

int main(int argc, char *argv[]) {
    char *start = ".";
    int i = 1;

    /* First non-option argument is the path */
    if (i < argc && argv[i][0] != '-')
        start = argv[i++];

    /* Parse expressions */
    while (i < argc) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            name_pat = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            type_filter = argv[i + 1][0];
            i += 2;
        } else if (strcmp(argv[i], "-maxdepth") == 0 && i + 1 < argc) {
            maxdepth = atoi(argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-print") == 0) {
            i++; /* -print is the default action */
        } else {
            dprintf(2, "find: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    walk(start, 0);
    return 0;
}
