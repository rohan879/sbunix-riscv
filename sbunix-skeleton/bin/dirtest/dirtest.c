#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] dir: %s\n", name); tests_passed++; }
    else { printf("[FAIL] dir: %s\n", name); tests_failed++; }
}

int main(void) {
    int r;

    /* mkdir */
    r = mkdir("/mnt/testdir", 0755);
    check(r == 0, "mkdir");

    /* create file */
    int fd = open("/mnt/testdir/file1", O_WRONLY | O_CREAT, 0644);
    check(fd >= 0, "create file in dir");
    write(fd, "test", 4);
    close(fd);

    /* opendir */
    DIR *d = opendir("/mnt/testdir");
    check(d != NULL, "opendir");

    /* readdir */
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, "file1") == 0) found = 1;
    }
    check(found, "readdir found file1");

    r = closedir(d);
    check(r == 0, "closedir");

    /* opendir root */
    d = opendir("/");
    check(d != NULL, "opendir /");
    found = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, "bin") == 0) found = 1;
    }
    check(found, "readdir / has bin");
    closedir(d);

    /* cleanup */
    unlink("/mnt/testdir/file1");
    rmdir("/mnt/testdir");

    printf("dirtest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
