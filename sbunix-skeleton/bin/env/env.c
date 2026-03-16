#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

extern char **environ;

int main(int argc, char *argv[]) {
    int iflag = 0;
    int i = 1;

    /* Parse options */
    if (i < argc && strcmp(argv[i], "-i") == 0) {
        iflag = 1;
        i++;
    }

    /* If -i, clear the environment */
    if (iflag) {
        static char *empty[] = { NULL };
        environ = empty;
    }

    /* Process NAME=VALUE pairs */
    while (i < argc && strchr(argv[i], '=') != NULL) {
        putenv(argv[i]);
        i++;
    }

    /* If no command, print environment */
    if (i >= argc) {
        if (environ) {
            for (char **ep = environ; *ep; ep++)
                printf("%s\n", *ep);
        }
        return 0;
    }

    /* Execute command */
    execvp(argv[i], &argv[i]);
    dprintf(2, "env: '%s': %s\n", argv[i], strerror(errno));
    return 127;
}
