#include <stdio.h>
#include <unistd.h>

static void cat_fd(int fd) {
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, n);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        cat_fd(STDIN_FILENO);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], 0);
        if (fd < 0) {
            printf("cat: %s: No such file or directory\n", argv[i]);
            return 1;
        }
        cat_fd(fd);
        close(fd);
    }
    return 0;
}
