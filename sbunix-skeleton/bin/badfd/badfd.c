#include <stdio.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
    char buf[16];
    int invalid_fd = 99; // Highly likely to be closed
    int ret;

    printf("Testing defensive syscalls on FD %d...\n", invalid_fd);

    errno = 0;
    ret = read(invalid_fd, buf, 10);
    printf("read() returned: %d, errno: %d (expected -1, 9)\n", ret, errno);

    errno = 0;
    ret = write(invalid_fd, "test", 4);
    printf("write() returned: %d, errno: %d (expected -1, 9)\n", ret, errno);

    errno = 0;
    ret = close(invalid_fd);
    printf("close() returned: %d, errno: %d (expected -1, 9)\n", ret, errno);

    return 0;
}
