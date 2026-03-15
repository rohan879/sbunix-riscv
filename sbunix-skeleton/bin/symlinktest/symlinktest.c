#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] symlink: %s\n", name); tests_passed++; }
    else { printf("[FAIL] symlink: %s\n", name); tests_failed++; }
}

int main(void) {
    /* create target */
    int fd = open("/mnt/sym_target", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create target");
    write(fd, "symdata", 7);
    close(fd);

    /* symlink */
    int r = symlink("/mnt/sym_target", "/mnt/sym_link");
    check(r == 0, "symlink created");

    /* readlink */
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int n = readlink("/mnt/sym_link", buf, sizeof(buf));
    check(n > 0, "readlink positive");
    check(strcmp(buf, "/mnt/sym_target") == 0, "readlink target correct");

    /* open through symlink */
    fd = open("/mnt/sym_link", O_RDONLY);
    check(fd >= 0, "open symlink");
    if (fd >= 0) {
        memset(buf, 0, sizeof(buf));
        read(fd, buf, 64);
        check(strcmp(buf, "symdata") == 0, "read through symlink");
        close(fd);
    }

    /* cleanup */
    unlink("/mnt/sym_link");
    unlink("/mnt/sym_target");

    printf("symlinktest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
