/*
 * persisttest.c -- Filesystem persistence tests across reboots
 *
 * This binary handles BOTH phases of persistence testing in one binary.
 * Phase detection: if the sentinel file already exists, we are in the
 * verify phase (Boot 2); otherwise we are in the write phase (Boot 1).
 *
 * Run from the shell:  persisttest
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/* -----------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* Read exactly 'count' bytes (retrying on short reads), return bytes read */
static int read_full(int fd, char *buf, int count) {
    int total = 0;
    while (total < count) {
        ssize_t nr = read(fd, buf + total, count - total);
        if (nr <= 0) break;
        total += (int)nr;
    }
    return total;
}

/* -----------------------------------------------------------------------
 * Sentinel file: used to detect which phase we are in.
 * ---------------------------------------------------------------------- */

#define SENTINEL_PATH   "/mnt/.persist_sentinel"
#define PERSIST_STRING  "PERSISTENCE_CHECK_12345"
#define PERSIST_LEN     23  /* strlen("PERSISTENCE_CHECK_12345") */

/* -----------------------------------------------------------------------
 * Phase 1 (write): create files and sync to disk
 * ---------------------------------------------------------------------- */

static void phase_write(void) {
    int fd;
    ssize_t nw;

    /* --- persist_write: simple string --- */
    {
        const char *path = "/mnt/persist_test";
        unlink(path);

        fd = open(path, O_CREAT | O_WRONLY, 0644);
        CHECK("persist_write: create file", fd >= 0);
        if (fd < 0) goto write_large;

        nw = write(fd, PERSIST_STRING, PERSIST_LEN);
        CHECK("persist_write: write data", nw == PERSIST_LEN);
        fsync(fd);
        close(fd);
    }

    printf("[PASS] persist_write\n");
    g_pass++;

write_large:
    /* --- persist_large: 4096 bytes of known pattern --- */
    {
        const char *path = "/mnt/persist_large";
        unsigned char pattern[4096];
        int i;

        for (i = 0; i < 4096; i++)
            pattern[i] = (unsigned char)(i % 251);

        unlink(path);
        fd = open(path, O_CREAT | O_WRONLY, 0644);
        CHECK("persist_large: create file", fd >= 0);
        if (fd < 0) goto write_mkdir;

        nw = write(fd, pattern, 4096);
        CHECK("persist_large: write 4096 bytes", nw == 4096);
        fsync(fd);
        close(fd);
    }

write_mkdir:
    /* --- persist_mkdir: create a directory --- */
    {
        const char *path = "/mnt/persist_dir";
        rmdir(path);

        int r = mkdir(path, 0755);
        CHECK("persist_mkdir: create directory", r == 0);
    }

    /* --- persist_multiple_files: create 3 files --- */
    {
        const char *paths[3] = {
            "/mnt/persist_multi_a",
            "/mnt/persist_multi_b",
            "/mnt/persist_multi_c"
        };
        const char *data[3] = {
            "alpha_content_111",
            "beta_content_222",
            "gamma_content_333"
        };
        int i;

        for (i = 0; i < 3; i++) {
            unlink(paths[i]);
            fd = open(paths[i], O_CREAT | O_WRONLY, 0644);
            if (fd < 0) {
                CHECK("persist_multiple_files: create file", 0);
                continue;
            }
            nw = write(fd, data[i], strlen(data[i]));
            fsync(fd);
            close(fd);
        }
        CHECK("persist_multiple_files: created 3 files", 1);
    }

    /* --- Write the sentinel last so verify phase knows everything is done --- */
    {
        fd = open(SENTINEL_PATH, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) {
            write(fd, "done", 4);
            fsync(fd);
            close(fd);
        }
    }

    /* Final sync to flush everything to disk */
    sync();
    printf("persist: write phase complete, reboot to verify\n");
}

/* -----------------------------------------------------------------------
 * Phase 2 (verify): read back files and check data integrity
 * ---------------------------------------------------------------------- */

static void phase_verify(void) {
    int fd;
    char buf[4096];

    /* --- persist_verify: simple string --- */
    {
        const char *path = "/mnt/persist_test";

        fd = open(path, O_RDONLY);
        CHECK("persist_verify: open file", fd >= 0);
        if (fd < 0) goto verify_large;

        memset(buf, 0, sizeof(buf));
        int nr = read_full(fd, buf, PERSIST_LEN);
        close(fd);

        CHECK("persist_verify: read correct length", nr == PERSIST_LEN);
        CHECK("persist_verify: data matches",
              memcmp(buf, PERSIST_STRING, PERSIST_LEN) == 0);
    }

    printf("[PASS] persist_verify\n");
    g_pass++;

verify_large:
    /* --- persist_large: verify 4096-byte pattern --- */
    {
        const char *path = "/mnt/persist_large";
        unsigned char rbuf[4096];

        fd = open(path, O_RDONLY);
        CHECK("persist_large: reopen file", fd >= 0);
        if (fd < 0) goto verify_mkdir;

        memset(rbuf, 0, sizeof(rbuf));
        int nr = read_full(fd, (char *)rbuf, 4096);
        close(fd);

        CHECK("persist_large: read 4096 bytes", nr == 4096);

        int match = 1;
        int i;
        for (i = 0; i < 4096; i++) {
            if (rbuf[i] != (unsigned char)(i % 251)) {
                match = 0;
                break;
            }
        }
        CHECK("persist_large: data integrity", match);
    }

verify_mkdir:
    /* --- persist_mkdir: verify directory still exists --- */
    {
        const char *path = "/mnt/persist_dir";
        struct stat st;
        int r = stat(path, &st);
        CHECK("persist_mkdir: directory exists", r == 0);
        CHECK("persist_mkdir: is a directory", r == 0 && S_ISDIR(st.st_mode));
    }

    /* --- persist_multiple_files: verify all 3 files --- */
    {
        const char *paths[3] = {
            "/mnt/persist_multi_a",
            "/mnt/persist_multi_b",
            "/mnt/persist_multi_c"
        };
        const char *expected[3] = {
            "alpha_content_111",
            "beta_content_222",
            "gamma_content_333"
        };
        int all_ok = 1;
        int i;

        for (i = 0; i < 3; i++) {
            fd = open(paths[i], O_RDONLY);
            if (fd < 0) {
                all_ok = 0;
                continue;
            }
            memset(buf, 0, sizeof(buf));
            int nr = read_full(fd, buf, (int)strlen(expected[i]));
            close(fd);

            if (nr != (int)strlen(expected[i]) ||
                memcmp(buf, expected[i], strlen(expected[i])) != 0) {
                all_ok = 0;
            }
        }
        CHECK("persist_multiple_files: all 3 files verified", all_ok);
    }

    /* --- Clean up the sentinel so next run starts fresh --- */
    unlink(SENTINEL_PATH);

    printf("persist: verify phase complete\n");
}

/* -----------------------------------------------------------------------
 * main — detect phase by checking for the sentinel file
 * ---------------------------------------------------------------------- */

int main(void) {
    struct stat st;

    printf("=== persisttest: filesystem persistence ===\n");

    if (stat(SENTINEL_PATH, &st) == 0) {
        /* Sentinel exists: we are in Boot 2 (verify phase) */
        printf("persist: sentinel found, entering VERIFY phase\n");
        phase_verify();
    } else {
        /* No sentinel: we are in Boot 1 (write phase) */
        printf("persist: no sentinel, entering WRITE phase\n");
        phase_write();
    }

    printf("=== persisttest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
