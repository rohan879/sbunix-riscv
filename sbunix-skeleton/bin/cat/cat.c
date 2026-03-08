#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

static void cat_fd(int fd) {
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, (size_t)n);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        cat_fd(STDIN_FILENO);
        return 0;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "cat: %s: %s\n", argv[i], strerror(errno));
            ret = 1;
            continue;
        }
        cat_fd(fd);
        close(fd);
    }
    return ret;
}
