#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        dprintf(2, "usage: realpath <path> ...\n");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        char resolved[256];
        char *r = realpath(argv[i], resolved);
        if (r)
            printf("%s\n", r);
        else {
            dprintf(2, "realpath: %s: No such file or directory\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
