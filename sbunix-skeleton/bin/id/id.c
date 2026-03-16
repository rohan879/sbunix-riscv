#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int uflag = 0, gflag = 0, nflag = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            dprintf(2, "id: unexpected argument '%s'\n", argv[i]);
            return 1;
        }
        for (int j = 1; argv[i][j]; j++) {
            switch (argv[i][j]) {
            case 'u': uflag = 1; break;
            case 'g': gflag = 1; break;
            case 'n': nflag = 1; break;
            default:
                dprintf(2, "id: unknown option '-%c'\n", argv[i][j]);
                return 1;
            }
        }
    }

    uid_t uid = getuid();
    gid_t gid = getgid();

    if (uflag) {
        if (nflag)
            printf("%s\n", uid == 0 ? "root" : "unknown");
        else
            printf("%d\n", (int)uid);
        return 0;
    }

    if (gflag) {
        if (nflag)
            printf("%s\n", gid == 0 ? "root" : "unknown");
        else
            printf("%d\n", (int)gid);
        return 0;
    }

    /* Default: full output */
    const char *uname = uid == 0 ? "root" : "unknown";
    const char *gname = gid == 0 ? "root" : "unknown";
    printf("uid=%d(%s) gid=%d(%s)\n", (int)uid, uname, (int)gid, gname);
    return 0;
}
