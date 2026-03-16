#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        dprintf(2, "usage: chown OWNER[:GROUP] FILE...\n");
        return 1;
    }

    /* Parse owner[:group] */
    char buf[256];
    strncpy(buf, argv[1], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uid_t owner = (uid_t)-1;
    gid_t group = (gid_t)-1;

    char *colon = strchr(buf, ':');
    if (colon) {
        *colon = '\0';
        if (colon[1] != '\0') {
            /* Group specified */
            if (strcmp(colon + 1, "root") == 0)
                group = 0;
            else
                group = (gid_t)atoi(colon + 1);
        }
    }

    if (buf[0] != '\0') {
        if (strcmp(buf, "root") == 0)
            owner = 0;
        else
            owner = (uid_t)atoi(buf);
    }

    int ret = 0;
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], owner, group) < 0) {
            dprintf(2, "chown: '%s': %s\n", argv[i], strerror(errno));
            ret = 1;
        }
    }
    return ret;
}
