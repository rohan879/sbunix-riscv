/*
 * symlinktest.c -- Symlink resolution tests
 *
 * Exercises symlink creation, readlink, open-through-symlink, symlink chains,
 * ELOOP detection, lstat, stat, relative targets, unlink, and dangling links.
 * All writable files use /mnt (ext2 filesystem).
 *
 * Run from the shell:  symlinktest
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* Write a string to a new file; returns 0 on success */
static int write_file(const char *path, const char *data, int len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    write(fd, data, len);
    close(fd);
    return 0;
}

/* Read file contents into buf, NUL-terminate, return bytes read or -1 */
static int read_file(const char *path, char *buf, int bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int total = 0;
    while (total < bufsz - 1) {
        int n = read(fd, buf + total, bufsz - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    buf[total] = '\0';
    return total;
}

/* Clean up test artifacts from prior runs */
static void cleanup_all(void) {
    unlink("/mnt/sl_link");
    unlink("/mnt/sl_target");
    unlink("/mnt/sl_dirlink");
    unlink("/mnt/sl_subdir/sl_inner");
    rmdir("/mnt/sl_subdir");
    unlink("/mnt/sl_chain_a");
    unlink("/mnt/sl_chain_b");
    unlink("/mnt/sl_chain_c");
    unlink("/mnt/sl_chain_real");
    unlink("/mnt/sl_loop_a");
    unlink("/mnt/sl_loop_b");
    unlink("/mnt/sl_regfile");
    unlink("/mnt/sl_stat_link");
    unlink("/mnt/sl_stat_target");
    unlink("/mnt/sl_rel_link");
    unlink("/mnt/sl_rel_target");
    unlink("/mnt/sl_unlink_link");
    unlink("/mnt/sl_unlink_target");
    unlink("/mnt/sl_dangle");
}

/* -----------------------------------------------------------------------
 * 1. symlink_create_readlink -- create symlink, readlink returns target
 * ---------------------------------------------------------------------- */

static void test_symlink_create_readlink(void) {
    unlink("/mnt/sl_link");
    unlink("/mnt/sl_target");

    write_file("/mnt/sl_target", "data", 4);

    int r = symlink("/mnt/sl_target", "/mnt/sl_link");
    CHECK("symlink: symlink_create_readlink create", r == 0);

    char buf[256];
    memset(buf, 0, sizeof(buf));
    int n = readlink("/mnt/sl_link", buf, sizeof(buf) - 1);
    CHECK("symlink: symlink_create_readlink readlink positive", n > 0);
    if (n > 0) {
        buf[n] = '\0';
        CHECK("symlink: symlink_create_readlink target matches",
              strcmp(buf, "/mnt/sl_target") == 0);
    }

    unlink("/mnt/sl_link");
    unlink("/mnt/sl_target");
}

/* -----------------------------------------------------------------------
 * 2. symlink_open_through -- open symlink, read content matches target
 * ---------------------------------------------------------------------- */

static void test_symlink_open_through(void) {
    const char *data = "hello_symlink";
    int dlen = 13;

    unlink("/mnt/sl_link");
    unlink("/mnt/sl_target");

    write_file("/mnt/sl_target", data, dlen);
    symlink("/mnt/sl_target", "/mnt/sl_link");

    int fd = open("/mnt/sl_link", O_RDONLY);
    CHECK("symlink: symlink_open_through open", fd >= 0);

    if (fd >= 0) {
        char buf[64];
        memset(buf, 0, sizeof(buf));
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        CHECK("symlink: symlink_open_through content matches",
              n == dlen && memcmp(buf, data, dlen) == 0);
    }

    unlink("/mnt/sl_link");
    unlink("/mnt/sl_target");
}

/* -----------------------------------------------------------------------
 * 3. symlink_to_directory -- symlink to dir, open file inside via link
 * ---------------------------------------------------------------------- */

static void test_symlink_to_directory(void) {
    unlink("/mnt/sl_subdir/sl_inner");
    rmdir("/mnt/sl_subdir");
    unlink("/mnt/sl_dirlink");

    mkdir("/mnt/sl_subdir", 0755);
    write_file("/mnt/sl_subdir/sl_inner", "inside", 6);
    symlink("/mnt/sl_subdir", "/mnt/sl_dirlink");

    char buf[64];
    int n = read_file("/mnt/sl_dirlink/sl_inner", buf, sizeof(buf));
    CHECK("symlink: symlink_to_directory read through",
          n == 6 && memcmp(buf, "inside", 6) == 0);

    unlink("/mnt/sl_subdir/sl_inner");
    rmdir("/mnt/sl_subdir");
    unlink("/mnt/sl_dirlink");
}

/* -----------------------------------------------------------------------
 * 4. symlink_chain -- A -> B -> C -> realfile (3-level chain)
 * ---------------------------------------------------------------------- */

static void test_symlink_chain(void) {
    unlink("/mnt/sl_chain_a");
    unlink("/mnt/sl_chain_b");
    unlink("/mnt/sl_chain_c");
    unlink("/mnt/sl_chain_real");

    write_file("/mnt/sl_chain_real", "chain_end", 9);
    symlink("/mnt/sl_chain_real", "/mnt/sl_chain_c");
    symlink("/mnt/sl_chain_c", "/mnt/sl_chain_b");
    symlink("/mnt/sl_chain_b", "/mnt/sl_chain_a");

    char buf[64];
    int n = read_file("/mnt/sl_chain_a", buf, sizeof(buf));
    CHECK("symlink: symlink_chain 3-level resolves",
          n == 9 && memcmp(buf, "chain_end", 9) == 0);

    unlink("/mnt/sl_chain_a");
    unlink("/mnt/sl_chain_b");
    unlink("/mnt/sl_chain_c");
    unlink("/mnt/sl_chain_real");
}

/* -----------------------------------------------------------------------
 * 5. symlink_loop_eloop -- A -> B -> A, open returns ELOOP
 * ---------------------------------------------------------------------- */

static void test_symlink_loop_eloop(void) {
    unlink("/mnt/sl_loop_a");
    unlink("/mnt/sl_loop_b");

    symlink("/mnt/sl_loop_b", "/mnt/sl_loop_a");
    symlink("/mnt/sl_loop_a", "/mnt/sl_loop_b");

    errno = 0;
    int fd = open("/mnt/sl_loop_a", O_RDONLY);
    CHECK("symlink: symlink_loop_eloop returns -1", fd == -1);
    CHECK("symlink: symlink_loop_eloop errno ELOOP", errno == ELOOP);
    if (fd >= 0) close(fd);

    unlink("/mnt/sl_loop_a");
    unlink("/mnt/sl_loop_b");
}

/* -----------------------------------------------------------------------
 * 6. readlink_not_symlink -- readlink on regular file returns -1/EINVAL
 * ---------------------------------------------------------------------- */

static void test_readlink_not_symlink(void) {
    unlink("/mnt/sl_regfile");
    write_file("/mnt/sl_regfile", "regular", 7);

    char buf[64];
    errno = 0;
    int n = readlink("/mnt/sl_regfile", buf, sizeof(buf));
    CHECK("symlink: readlink_not_symlink returns -1", n == -1);
    CHECK("symlink: readlink_not_symlink errno EINVAL", errno == EINVAL);

    unlink("/mnt/sl_regfile");
}

/* -----------------------------------------------------------------------
 * 7. stat_follows_symlink -- stat on symlink returns target's info
 * ---------------------------------------------------------------------- */

static void test_stat_follows_symlink(void) {
    const char *data = "stat_target_data";
    int dlen = 16;

    unlink("/mnt/sl_stat_link");
    unlink("/mnt/sl_stat_target");

    write_file("/mnt/sl_stat_target", data, dlen);
    symlink("/mnt/sl_stat_target", "/mnt/sl_stat_link");

    struct stat st_target, st_link;
    int r1 = stat("/mnt/sl_stat_target", &st_target);
    int r2 = stat("/mnt/sl_stat_link", &st_link);

    CHECK("symlink: stat_follows_symlink both stat ok",
          r1 == 0 && r2 == 0);
    if (r1 == 0 && r2 == 0) {
        CHECK("symlink: stat_follows_symlink size matches",
              st_link.st_size == st_target.st_size);
        CHECK("symlink: stat_follows_symlink is regular file",
              S_ISREG(st_link.st_mode));
    }

    unlink("/mnt/sl_stat_link");
    unlink("/mnt/sl_stat_target");
}

/* -----------------------------------------------------------------------
 * 8. lstat_no_follow -- lstat returns symlink's info (S_ISLNK)
 * ---------------------------------------------------------------------- */

static void test_lstat_no_follow(void) {
    unlink("/mnt/sl_stat_link");
    unlink("/mnt/sl_stat_target");

    write_file("/mnt/sl_stat_target", "lstat_data", 10);
    symlink("/mnt/sl_stat_target", "/mnt/sl_stat_link");

    struct stat lst;
    int r = lstat("/mnt/sl_stat_link", &lst);
    CHECK("symlink: lstat_no_follow returns 0", r == 0);
    if (r == 0) {
        CHECK("symlink: lstat_no_follow S_ISLNK", S_ISLNK(lst.st_mode));
    }

    unlink("/mnt/sl_stat_link");
    unlink("/mnt/sl_stat_target");
}

/* -----------------------------------------------------------------------
 * 9. symlink_relative_target -- symlink with relative target resolves
 * ---------------------------------------------------------------------- */

static void test_symlink_relative_target(void) {
    unlink("/mnt/sl_rel_link");
    unlink("/mnt/sl_rel_target");

    write_file("/mnt/sl_rel_target", "rel_ok", 6);

    /* Create symlink with relative target: "sl_rel_target" (no leading /) */
    int r = symlink("sl_rel_target", "/mnt/sl_rel_link");
    CHECK("symlink: symlink_relative_target create", r == 0);

    /* Verify readlink returns the relative target string */
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int n = readlink("/mnt/sl_rel_link", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0';
    CHECK("symlink: symlink_relative_target readlink is relative",
          n > 0 && strcmp(buf, "sl_rel_target") == 0);

    /* Opening through the symlink should resolve relative to /mnt/ */
    char rbuf[64];
    n = read_file("/mnt/sl_rel_link", rbuf, sizeof(rbuf));
    CHECK("symlink: symlink_relative_target open resolves",
          n == 6 && memcmp(rbuf, "rel_ok", 6) == 0);

    unlink("/mnt/sl_rel_link");
    unlink("/mnt/sl_rel_target");
}

/* -----------------------------------------------------------------------
 * 10. symlink_unlink -- unlink symlink doesn't remove target file
 * ---------------------------------------------------------------------- */

static void test_symlink_unlink(void) {
    unlink("/mnt/sl_unlink_link");
    unlink("/mnt/sl_unlink_target");

    write_file("/mnt/sl_unlink_target", "survive", 7);
    symlink("/mnt/sl_unlink_target", "/mnt/sl_unlink_link");

    /* Unlink the symlink itself */
    int r = unlink("/mnt/sl_unlink_link");
    CHECK("symlink: symlink_unlink unlink returns 0", r == 0);

    /* Symlink should be gone */
    int fd = open("/mnt/sl_unlink_link", O_RDONLY);
    CHECK("symlink: symlink_unlink link gone", fd < 0);
    if (fd >= 0) close(fd);

    /* Target should still exist and be readable */
    char buf[64];
    int n = read_file("/mnt/sl_unlink_target", buf, sizeof(buf));
    CHECK("symlink: symlink_unlink target survives",
          n == 7 && memcmp(buf, "survive", 7) == 0);

    unlink("/mnt/sl_unlink_target");
}

/* -----------------------------------------------------------------------
 * 11. symlink_dangling -- symlink to nonexistent file, open returns ENOENT
 * ---------------------------------------------------------------------- */

static void test_symlink_dangling(void) {
    unlink("/mnt/sl_dangle");
    unlink("/mnt/sl_noexist");

    symlink("/mnt/sl_noexist", "/mnt/sl_dangle");

    errno = 0;
    int fd = open("/mnt/sl_dangle", O_RDONLY);
    CHECK("symlink: symlink_dangling returns -1", fd == -1);
    CHECK("symlink: symlink_dangling errno ENOENT", errno == ENOENT);
    if (fd >= 0) close(fd);

    unlink("/mnt/sl_dangle");
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== symlinktest: symlink resolution ===\n");

    /* Clean up any leftovers from prior runs */
    cleanup_all();

    test_symlink_create_readlink();
    test_symlink_open_through();
    test_symlink_to_directory();
    test_symlink_chain();
    test_symlink_loop_eloop();
    test_readlink_not_symlink();
    test_stat_follows_symlink();
    test_lstat_no_follow();
    test_symlink_relative_target();
    test_symlink_unlink();
    test_symlink_dangling();

    printf("=== symlinktest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
