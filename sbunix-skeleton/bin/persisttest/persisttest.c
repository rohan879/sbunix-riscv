#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

static const char *persist_path = "/mnt/persist_test";
static const char *persist_data = "persistence-check-data-12345";

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] persist: %s\n", name); tests_passed++; }
    else { printf("[FAIL] persist: %s\n", name); tests_failed++; }
}

static int run_write_phase(void) {
    int fd = open(persist_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open for write");
    if (fd < 0)
        return 1;

    int len = strlen(persist_data);
    int n = write(fd, persist_data, len);
    check(n == len, "write data");
    check(fsync(fd) == 0, "fsync write phase");
    close(fd);
    return tests_failed > 0 ? 1 : 0;
}

static int run_verify_phase(void) {
    char buf[64];
    int fd = open(persist_path, O_RDONLY);
    check(fd >= 0, "open for verify");
    if (fd < 0)
        return 1;

    memset(buf, 0, sizeof(buf));
    int n = read(fd, buf, sizeof(buf));
    close(fd);

    int len = strlen(persist_data);
    check(n == len, "read persisted length");
    check(strcmp(buf, persist_data) == 0, "read persisted data");

    fd = open(persist_path, O_WRONLY | O_APPEND);
    check(fd >= 0, "open for append");
    if (fd >= 0) {
        check(write(fd, "!", 1) == 1, "append marker");
        check(fsync(fd) == 0, "fsync verify phase");
        close(fd);
    }

    fd = open(persist_path, O_RDONLY);
    check(fd >= 0, "reopen after append");
    if (fd >= 0) {
        memset(buf, 0, sizeof(buf));
        n = read(fd, buf, sizeof(buf));
        close(fd);
        check(n == len + 1, "read appended length");
        check(strcmp(buf, "persistence-check-data-12345!") == 0,
              "read appended data");
    }

    unlink(persist_path);
    return tests_failed > 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: persisttest <write|verify>\n");
        return 1;
    }

    if (strcmp(argv[1], "write") == 0)
        return run_write_phase();
    if (strcmp(argv[1], "verify") == 0)
        return run_verify_phase();

    printf("usage: persisttest <write|verify>\n");
    return 1;
}
