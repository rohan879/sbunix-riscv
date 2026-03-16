#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        dprintf(2, "usage: rmdir DIR...\n");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            dprintf(2, "rmdir: '%s': %s\n", argv[i], strerror(errno));
            ret = 1;
        }
    }
    return ret;
}
