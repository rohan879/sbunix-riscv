#include <stdio.h>
#include <sys/utsname.h>

int main(void) {
    struct utsname buf;
    if (uname(&buf) < 0) {
        printf("uname: failed\n");
        return 1;
    }
    printf("%s %s %s %s %s\n",
           buf.sysname, buf.nodename, buf.release, buf.version, buf.machine);
    return 0;
}
