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
#include <sys/types.h>
#include <dirent.h>

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
    unlink("/mnt/sl_rp_dir/link");
    unlink("/mnt/sl_rp_dir/target");
    rmdir("/mnt/sl_rp_dir");
    unlink("/mnt/sl_rp_file");
    unlink("/mnt/sl_rp_rel_parent/up");
    unlink("/mnt/sl_rp_rel_sib/file");
    rmdir("/mnt/sl_rp_rel_parent");
    rmdir("/mnt/sl_rp_rel_sib");
    unlink("/mnt/sl_rp_loop_a");
    unlink("/mnt/sl_rp_loop_b");
    unlink("/mnt/sl_unlink_link");
    unlink("/mnt/sl_unlink_target");
    unlink("/mnt/sl_dangle");
    unlink("/mnt/sl_dangle_create");
    unlink("/mnt/sl_dangle_create_target");
    unlink("/mnt/sl_rla_file");
    unlink("/mnt/sl_rla_dir/child");
    unlink("/mnt/sl_rla_dir/child_target");
    rmdir("/mnt/sl_rla_dir");
    unlink("/mnt/sl_rla_dirlink");
    unlink("/mnt/sl_sat_parent_link/child");
    rmdir("/mnt/sl_sat_parent_link");
    rmdir("/mnt/sl_sat_parent_real");
    unlink("/mnt/sl_sat_parent_sym");
    unlink("/mnt/sl_long_target_link");
    unlink("/tmp/sl_ramfs_long_link");
    unlink("/mnt/sl_empty_target_link");
    unlink("/mnt/sl_zero_target_link");
    unlink("/mnt/sl_zero_target_file");
    unlink("/mnt/sl_symlink_trailing_link");
    rmdir("/mnt/sl_symlink_trailing_dir");
    unlink("/mnt/sl_nofollow_link");
    unlink("/mnt/sl_nofollow_target");
    unlink("/mnt/sl_overlong_link");
    unlink("/tmp/sl_linkat_hard");
    unlink("/tmp/sl_linkat_sym");
    unlink("/tmp/sl_linkat_target");
    unlink("/mnt/sl_rename_src_link");
    rmdir("/mnt/sl_rename_src_target");
    rmdir("/mnt/sl_rename_src_new");
    unlink("/mnt/sl_rename_dst_file");
    unlink("/mnt/sl_rename_dst_link");
    rmdir("/mnt/sl_rename_dst_target");
    unlink("/tmp/sl_nl_devlink");
    unlink("/tmp/sl_nl_binlink");
    unlink("/tmp/sl_nl_syslink");
    unlink("/tmp/sl_nl_proclink");
    unlink("/tmp/sl_nl_rla_bin");
    unlink("/tmp/sl_nl_rla_procself");
    unlink("/tmp/sl_nl_ramfs_file");
    rmdir("/tmp/sl_nl_ramfs_dir");
    unlink("/tmp/sl_rla_file");
    rmdir("/tmp/sl_rla_dir");
    unlink("/tmp/sl_nl_ramfs_parent_real/file");
    rmdir("/tmp/sl_nl_ramfs_parent_real/dir");
    unlink("/tmp/sl_nl_ramfs_parent_link");
    rmdir("/tmp/sl_nl_ramfs_parent_real");
    unlink("/mnt/sl_dtype_link");
    unlink("/mnt/sl_nl_parent_real/file");
    rmdir("/mnt/sl_nl_parent_real/dir");
    unlink("/mnt/sl_nl_parent_link");
    rmdir("/mnt/sl_nl_parent_real");
    rmdir("/mnt/sl_nl_dir");
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
 * 6b. readlink_nonlink_variants -- non-links report -1/EINVAL consistently
 * ---------------------------------------------------------------------- */

static void test_readlink_nonlink_variants(void) {
    char buf[64];

    cleanup_all();

    CHECK("symlink: readlink_nonlink_variants dir setup",
          mkdir("/mnt/sl_nl_dir", 0755) == 0);
    errno = 0;
    CHECK("symlink: readlink_nonlink_variants dir errno EINVAL",
          readlink("/mnt/sl_nl_dir", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    CHECK("symlink: readlink_nonlink_variants parent setup",
          mkdir("/mnt/sl_nl_parent_real", 0755) == 0 &&
          mkdir("/mnt/sl_nl_parent_real/dir", 0755) == 0 &&
          write_file("/mnt/sl_nl_parent_real/file", "x", 1) == 0 &&
          symlink("/mnt/sl_nl_parent_real", "/mnt/sl_nl_parent_link") == 0);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants file via parent symlink EINVAL",
          readlink("/mnt/sl_nl_parent_link/file", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants dir via parent symlink EINVAL",
          readlink("/mnt/sl_nl_parent_link/dir", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    CHECK("symlink: readlink_nonlink_variants dev parent symlink setup",
          symlink("/dev", "/tmp/sl_nl_devlink") == 0);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants dev file EINVAL",
          readlink("/dev/null", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants dev file via parent symlink EINVAL",
          readlink("/tmp/sl_nl_devlink/null", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants dev missing via parent symlink ENOENT",
          readlink("/tmp/sl_nl_devlink/no-such", buf, sizeof(buf)) == -1 &&
          errno == ENOENT);

    CHECK("symlink: readlink_nonlink_variants ramfs file setup",
          write_file("/tmp/sl_nl_ramfs_file", "x", 1) == 0);
    errno = 0;
    CHECK("symlink: readlink_nonlink_variants ramfs file EINVAL",
          readlink("/tmp/sl_nl_ramfs_file", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    CHECK("symlink: readlink_nonlink_variants ramfs dir setup",
          mkdir("/tmp/sl_nl_ramfs_dir", 0755) == 0);
    errno = 0;
    CHECK("symlink: readlink_nonlink_variants ramfs dir EINVAL",
          readlink("/tmp/sl_nl_ramfs_dir", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    CHECK("symlink: readlink_nonlink_variants ramfs parent setup",
          mkdir("/tmp/sl_nl_ramfs_parent_real", 0755) == 0 &&
          mkdir("/tmp/sl_nl_ramfs_parent_real/dir", 0755) == 0 &&
          write_file("/tmp/sl_nl_ramfs_parent_real/file", "x", 1) == 0 &&
          symlink("/tmp/sl_nl_ramfs_parent_real",
                  "/tmp/sl_nl_ramfs_parent_link") == 0);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants ramfs file via parent symlink EINVAL",
          readlink("/tmp/sl_nl_ramfs_parent_link/file", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants ramfs dir via parent symlink EINVAL",
          readlink("/tmp/sl_nl_ramfs_parent_link/dir", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants ramfs missing via parent symlink ENOENT",
          readlink("/tmp/sl_nl_ramfs_parent_link/no-such", buf, sizeof(buf)) == -1 &&
          errno == ENOENT);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants tarfs file EINVAL",
          readlink("/bin/echo", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants tarfs dir EINVAL",
          readlink("/bin", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    CHECK("symlink: readlink_nonlink_variants tarfs parent symlink setup",
          symlink("/bin", "/tmp/sl_nl_binlink") == 0);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants tarfs file via parent symlink EINVAL",
          readlink("/tmp/sl_nl_binlink/echo", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants tarfs missing via parent symlink ENOENT",
          readlink("/tmp/sl_nl_binlink/no-such", buf, sizeof(buf)) == -1 &&
          errno == ENOENT);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants sysfs file EINVAL",
          readlink("/sys/kernel/version", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants sysfs dir EINVAL",
          readlink("/sys/devices", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    CHECK("symlink: readlink_nonlink_variants sysfs parent symlink setup",
          symlink("/sys", "/tmp/sl_nl_syslink") == 0);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants sysfs file via parent symlink EINVAL",
          readlink("/tmp/sl_nl_syslink/kernel/version", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants sysfs missing via parent symlink ENOENT",
          readlink("/tmp/sl_nl_syslink/no-such", buf, sizeof(buf)) == -1 &&
          errno == ENOENT);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants procfs file EINVAL",
          readlink("/proc/self/status", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants procfs dir EINVAL",
          readlink("/proc/self/fd", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    CHECK("symlink: readlink_nonlink_variants procfs parent symlink setup",
          symlink("/proc/self", "/tmp/sl_nl_proclink") == 0);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants procfs file via parent symlink EINVAL",
          readlink("/tmp/sl_nl_proclink/status", buf, sizeof(buf)) == -1 &&
          errno == EINVAL);

    errno = 0;
    CHECK("symlink: readlink_nonlink_variants procfs missing via parent symlink ENOENT",
          readlink("/tmp/sl_nl_proclink/no-such", buf, sizeof(buf)) == -1 &&
          errno == ENOENT);

    unlink("/mnt/sl_nl_parent_real/file");
    rmdir("/mnt/sl_nl_parent_real/dir");
    unlink("/mnt/sl_nl_parent_link");
    rmdir("/mnt/sl_nl_parent_real");
    unlink("/tmp/sl_nl_devlink");
    unlink("/tmp/sl_nl_binlink");
    unlink("/tmp/sl_nl_syslink");
    unlink("/tmp/sl_nl_proclink");
    unlink("/tmp/sl_nl_ramfs_file");
    rmdir("/tmp/sl_nl_ramfs_dir");
    unlink("/tmp/sl_nl_ramfs_parent_real/file");
    rmdir("/tmp/sl_nl_ramfs_parent_real/dir");
    unlink("/tmp/sl_nl_ramfs_parent_link");
    rmdir("/tmp/sl_nl_ramfs_parent_real");
    rmdir("/mnt/sl_nl_dir");
}

/* -----------------------------------------------------------------------
 * 6c. readlinkat_nonlink_variants -- relative readlinkat also rejects non-links
 * ---------------------------------------------------------------------- */

static void test_readlinkat_nonlink_variants(void) {
    char buf[64];
    int dirfd;

    cleanup_all();

    CHECK("symlink: readlinkat_nonlink_variants ext2 setup",
          write_file("/mnt/sl_rla_file", "x", 1) == 0 &&
          mkdir("/mnt/sl_rla_dir", 0755) == 0);
    dirfd = open("/mnt", O_RDONLY | O_DIRECTORY);
    CHECK("symlink: readlinkat_nonlink_variants ext2 dirfd open", dirfd >= 0);
    if (dirfd >= 0) {
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants ext2 file EINVAL",
              readlinkat(dirfd, "sl_rla_file", buf, sizeof(buf)) == -1 &&
              errno == EINVAL);
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants ext2 dir EINVAL",
              readlinkat(dirfd, "sl_rla_dir", buf, sizeof(buf)) == -1 &&
              errno == EINVAL);
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants ext2 missing ENOENT",
              readlinkat(dirfd, "sl_rla_missing", buf, sizeof(buf)) == -1 &&
              errno == ENOENT);
        close(dirfd);
    }

    CHECK("symlink: readlinkat_nonlink_variants ramfs setup",
          write_file("/tmp/sl_rla_file", "x", 1) == 0 &&
          mkdir("/tmp/sl_rla_dir", 0755) == 0);
    dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    CHECK("symlink: readlinkat_nonlink_variants ramfs dirfd open", dirfd >= 0);
    if (dirfd >= 0) {
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants ramfs file EINVAL",
              readlinkat(dirfd, "sl_rla_file", buf, sizeof(buf)) == -1 &&
              errno == EINVAL);
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants ramfs dir EINVAL",
              readlinkat(dirfd, "sl_rla_dir", buf, sizeof(buf)) == -1 &&
              errno == EINVAL);
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants ramfs missing ENOENT",
              readlinkat(dirfd, "sl_rla_missing", buf, sizeof(buf)) == -1 &&
              errno == ENOENT);
        close(dirfd);
    }

    dirfd = open("/bin", O_RDONLY | O_DIRECTORY);
    CHECK("symlink: readlinkat_nonlink_variants tarfs dirfd open", dirfd >= 0);
    if (dirfd >= 0) {
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants tarfs file EINVAL",
              readlinkat(dirfd, "echo", buf, sizeof(buf)) == -1 &&
              errno == EINVAL);
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants tarfs missing ENOENT",
              readlinkat(dirfd, "no-such", buf, sizeof(buf)) == -1 &&
              errno == ENOENT);
        close(dirfd);
    }

    dirfd = open("/sys/kernel", O_RDONLY | O_DIRECTORY);
    CHECK("symlink: readlinkat_nonlink_variants sysfs dirfd open", dirfd >= 0);
    if (dirfd >= 0) {
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants sysfs file EINVAL",
              readlinkat(dirfd, "version", buf, sizeof(buf)) == -1 &&
              errno == EINVAL);
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants sysfs missing ENOENT",
              readlinkat(dirfd, "no-such", buf, sizeof(buf)) == -1 &&
              errno == ENOENT);
        close(dirfd);
    }

    dirfd = open("/proc/self", O_RDONLY | O_DIRECTORY);
    CHECK("symlink: readlinkat_nonlink_variants procfs dirfd open", dirfd >= 0);
    if (dirfd >= 0) {
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants procfs file EINVAL",
              readlinkat(dirfd, "status", buf, sizeof(buf)) == -1 &&
              errno == EINVAL);
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants procfs missing ENOENT",
              readlinkat(dirfd, "no-such", buf, sizeof(buf)) == -1 &&
              errno == ENOENT);
        close(dirfd);
    }

    CHECK("symlink: readlinkat_nonlink_variants tarfs symlinked dirfd setup",
          symlink("/bin", "/tmp/sl_nl_rla_bin") == 0);
    dirfd = open("/tmp/sl_nl_rla_bin", O_RDONLY | O_DIRECTORY);
    CHECK("symlink: readlinkat_nonlink_variants tarfs symlinked dirfd open",
          dirfd >= 0);
    if (dirfd >= 0) {
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants tarfs symlinked file EINVAL",
              readlinkat(dirfd, "echo", buf, sizeof(buf)) == -1 &&
              errno == EINVAL);
        close(dirfd);
    }

    CHECK("symlink: readlinkat_nonlink_variants procfs symlinked dirfd setup",
          symlink("/proc/self", "/tmp/sl_nl_rla_procself") == 0);
    dirfd = open("/tmp/sl_nl_rla_procself", O_RDONLY | O_DIRECTORY);
    CHECK("symlink: readlinkat_nonlink_variants procfs symlinked dirfd open",
          dirfd >= 0);
    if (dirfd >= 0) {
        errno = 0;
        CHECK("symlink: readlinkat_nonlink_variants procfs symlinked file EINVAL",
              readlinkat(dirfd, "status", buf, sizeof(buf)) == -1 &&
              errno == EINVAL);
        close(dirfd);
    }

    unlink("/mnt/sl_rla_file");
    rmdir("/mnt/sl_rla_dir");
    unlink("/tmp/sl_rla_file");
    rmdir("/tmp/sl_rla_dir");
    unlink("/tmp/sl_nl_rla_bin");
    unlink("/tmp/sl_nl_rla_procself");
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
 * 10. realpath_relative_symlink_parent -- keep parent for relative target
 * ---------------------------------------------------------------------- */

static void test_realpath_relative_symlink_parent(void) {
    const char *expected = "/mnt/sl_rp_dir/target";

    unlink("/mnt/sl_rp_dir/link");
    unlink("/mnt/sl_rp_dir/target");
    rmdir("/mnt/sl_rp_dir");

    CHECK("symlink: realpath_relative_symlink_parent mkdir",
          mkdir("/mnt/sl_rp_dir", 0755) == 0);
    CHECK("symlink: realpath_relative_symlink_parent target",
          write_file(expected, "rp", 2) == 0);
    CHECK("symlink: realpath_relative_symlink_parent link",
          symlink("target", "/mnt/sl_rp_dir/link") == 0);

    /* BF: realpath keeps the containing directory in its resolved prefix while
     * it replays a relative symlink target, so /dir/link -> target becomes
     * /dir/target rather than resolving target from the process cwd. */
    char resolved[256];
    memset(resolved, 0, sizeof(resolved));
    char *rp = realpath("/mnt/sl_rp_dir/link", resolved);
    CHECK("symlink: realpath_relative_symlink_parent static buffer",
          rp == resolved && strcmp(resolved, expected) == 0);

    rp = realpath("/mnt/sl_rp_dir/link", NULL);
    CHECK("symlink: realpath_relative_symlink_parent allocated buffer",
          rp != NULL && strcmp(rp, expected) == 0);
    if (rp)
        free(rp);

    unlink("/mnt/sl_rp_dir/link");
    unlink("/mnt/sl_rp_dir/target");
    rmdir("/mnt/sl_rp_dir");
}

/* -----------------------------------------------------------------------
 * 10b. realpath_errno_edges -- missing paths and nondirectory prefixes fail
 * ---------------------------------------------------------------------- */

static void test_realpath_errno_edges(void) {
    char resolved[256];
    char *rp;

    unlink("/mnt/sl_rp_file");

    errno = 0;
    rp = realpath("/mnt/sl_rp_missing", resolved);
    CHECK("symlink: realpath_errno_edges missing returns NULL", rp == NULL);
    CHECK("symlink: realpath_errno_edges missing errno ENOENT",
          errno == ENOENT);

    CHECK("symlink: realpath_errno_edges file setup",
          write_file("/mnt/sl_rp_file", "x", 1) == 0);

    errno = 0;
    rp = realpath("/mnt/sl_rp_file/child", resolved);
    CHECK("symlink: realpath_errno_edges nondir returns NULL", rp == NULL);
    CHECK("symlink: realpath_errno_edges nondir errno ENOTDIR",
          errno == ENOTDIR);

    unlink("/mnt/sl_rp_file");
}

/* -----------------------------------------------------------------------
 * 10c. realpath_complex_edges -- relative .. targets, loops, trailing slash
 * ---------------------------------------------------------------------- */

static void test_realpath_complex_edges(void) {
    char resolved[256];
    char *rp;

    cleanup_all();

    CHECK("symlink: realpath_complex_edges parent mkdir",
          mkdir("/mnt/sl_rp_rel_parent", 0755) == 0);
    CHECK("symlink: realpath_complex_edges sibling mkdir",
          mkdir("/mnt/sl_rp_rel_sib", 0755) == 0);
    CHECK("symlink: realpath_complex_edges sibling file",
          write_file("/mnt/sl_rp_rel_sib/file", "z", 1) == 0);
    CHECK("symlink: realpath_complex_edges relative dotdot link",
          symlink("../sl_rp_rel_sib", "/mnt/sl_rp_rel_parent/up") == 0);

    errno = 0;
    rp = realpath("/mnt/sl_rp_rel_parent/up/file", resolved);
    CHECK("symlink: realpath_complex_edges relative dotdot resolves",
          rp == resolved && strcmp(resolved, "/mnt/sl_rp_rel_sib/file") == 0);

    errno = 0;
    rp = realpath("/mnt/sl_rp_rel_sib/file/", resolved);
    CHECK("symlink: realpath_complex_edges trailing slash on file NULL",
          rp == NULL);
    CHECK("symlink: realpath_complex_edges trailing slash on file ENOTDIR",
          errno == ENOTDIR);

    CHECK("symlink: realpath_complex_edges loop setup",
          symlink("/mnt/sl_rp_loop_b", "/mnt/sl_rp_loop_a") == 0 &&
          symlink("/mnt/sl_rp_loop_a", "/mnt/sl_rp_loop_b") == 0);
    errno = 0;
    rp = realpath("/mnt/sl_rp_loop_a", resolved);
    CHECK("symlink: realpath_complex_edges loop returns NULL", rp == NULL);
    CHECK("symlink: realpath_complex_edges loop errno ELOOP",
          errno == ELOOP);

    unlink("/mnt/sl_rp_rel_parent/up");
    unlink("/mnt/sl_rp_rel_sib/file");
    rmdir("/mnt/sl_rp_rel_parent");
    rmdir("/mnt/sl_rp_rel_sib");
    unlink("/mnt/sl_rp_loop_a");
    unlink("/mnt/sl_rp_loop_b");
}

/* -----------------------------------------------------------------------
 * 11. symlink_unlink -- unlink symlink doesn't remove target file
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
 * 12. symlink_dangling -- symlink to nonexistent file, open returns ENOENT
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
 * symlink_dangling_create -- O_CREAT follows dangling final target
 * ---------------------------------------------------------------------- */

static void test_symlink_dangling_create(void) {
    struct stat st;
    char buf[64];

    unlink("/mnt/sl_dangle_create");
    unlink("/mnt/sl_dangle_create_target");

    CHECK("symlink: symlink_dangling_create setup",
          symlink("/mnt/sl_dangle_create_target",
                  "/mnt/sl_dangle_create") == 0);

    errno = 0;
    int fd = open("/mnt/sl_dangle_create",
                  O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK("symlink: symlink_dangling_create excl returns -1", fd == -1);
    CHECK("symlink: symlink_dangling_create excl errno EEXIST",
          errno == EEXIST);
    if (fd >= 0) close(fd);
    CHECK("symlink: symlink_dangling_create excl target absent",
          stat("/mnt/sl_dangle_create_target", &st) == -1 &&
          errno == ENOENT);

    errno = 0;
    fd = open("/mnt/sl_dangle_create", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK("symlink: symlink_dangling_create open creates target", fd >= 0);
    if (fd >= 0) {
        CHECK("symlink: symlink_dangling_create write target",
              write(fd, "made", 4) == 4);
        close(fd);
    }

    memset(buf, 0, sizeof(buf));
    int n = read_file("/mnt/sl_dangle_create_target", buf, sizeof(buf));
    CHECK("symlink: symlink_dangling_create target content",
          n == 4 && memcmp(buf, "made", 4) == 0);

    CHECK("symlink: symlink_dangling_create link preserved",
          lstat("/mnt/sl_dangle_create", &st) == 0 && S_ISLNK(st.st_mode));

    unlink("/mnt/sl_dangle_create");
    unlink("/mnt/sl_dangle_create_target");
}

/* -----------------------------------------------------------------------
 * 13. readlink_intermediate_dir_symlink -- follow dirname symlink only
 * ---------------------------------------------------------------------- */

static void test_readlink_intermediate_dir_symlink(void) {
    unlink("/mnt/sl_rla_dir/child");
    unlink("/mnt/sl_rla_dir/child_target");
    rmdir("/mnt/sl_rla_dir");
    unlink("/mnt/sl_rla_dirlink");

    mkdir("/mnt/sl_rla_dir", 0755);
    write_file("/mnt/sl_rla_dir/child_target", "x", 1);
    symlink("child_target", "/mnt/sl_rla_dir/child");
    symlink("/mnt/sl_rla_dir", "/mnt/sl_rla_dirlink");

    char buf[256];
    memset(buf, 0, sizeof(buf));
    errno = 0;
    int n = readlink("/mnt/sl_rla_dirlink/child", buf, sizeof(buf) - 1);
    CHECK("symlink: readlink_intermediate_dir_symlink returns data", n > 0);
    if (n > 0) {
        buf[n] = '\0';
        CHECK("symlink: readlink_intermediate_dir_symlink target matches",
              strcmp(buf, "child_target") == 0);
    } else {
        CHECK("symlink: readlink_intermediate_dir_symlink errno clear", errno == 0);
    }

    unlink("/mnt/sl_rla_dir/child");
    unlink("/mnt/sl_rla_dir/child_target");
    rmdir("/mnt/sl_rla_dir");
    unlink("/mnt/sl_rla_dirlink");
}

/* -----------------------------------------------------------------------
 * 14. symlinkat_parent_symlink -- create link through symlinked dirname
 * ---------------------------------------------------------------------- */

static void test_symlinkat_parent_symlink(void) {
    unlink("/mnt/sl_sat_parent_link/child");
    rmdir("/mnt/sl_sat_parent_link");
    rmdir("/mnt/sl_sat_parent_real");
    unlink("/mnt/sl_sat_parent_sym");

    mkdir("/mnt/sl_sat_parent_real", 0755);
    symlink("/mnt/sl_sat_parent_real", "/mnt/sl_sat_parent_sym");

    int r = symlink("target", "/mnt/sl_sat_parent_sym/child");
    CHECK("symlink: symlinkat_parent_symlink create through parent link", r == 0);

    char buf[128];
    memset(buf, 0, sizeof(buf));
    int n = readlink("/mnt/sl_sat_parent_real/child", buf, sizeof(buf) - 1);
    CHECK("symlink: symlinkat_parent_symlink appears in real dir", n > 0);
    if (n > 0) {
        buf[n] = '\0';
        CHECK("symlink: symlinkat_parent_symlink target matches",
              strcmp(buf, "target") == 0);
    }

    unlink("/mnt/sl_sat_parent_real/child");
    rmdir("/mnt/sl_sat_parent_real");
    unlink("/mnt/sl_sat_parent_sym");
}

/* -----------------------------------------------------------------------
 * 15. symlink_long_target -- long target strings survive symlink/readlink
 * ---------------------------------------------------------------------- */

static void test_symlink_long_target(void) {
    char target[220];
    char buf[256];
    int pos = 0;

    unlink("/mnt/sl_long_target_link");

    memset(target, 0, sizeof(target));
    for (int i = 0; i < 20; i++) {
        target[pos++] = '/';
        target[pos++] = 'd';
        target[pos++] = 'i';
        target[pos++] = 'r';
        target[pos++] = '0' + (i % 10);
        target[pos++] = 'x';
        target[pos++] = 'y';
        target[pos++] = 'z';
    }
    target[pos] = '\0';

    CHECK("symlink: symlink_long_target create",
          symlink(target, "/mnt/sl_long_target_link") == 0);

    memset(buf, 0, sizeof(buf));
    errno = 0;
    int n = readlink("/mnt/sl_long_target_link", buf, sizeof(buf) - 1);
    CHECK("symlink: symlink_long_target readlink returns full target",
          n == pos);
    if (n > 0) {
        buf[n] = '\0';
        CHECK("symlink: symlink_long_target target matches",
              strcmp(buf, target) == 0);
    }

    unlink("/mnt/sl_long_target_link");
}

/* -----------------------------------------------------------------------
 * 16. symlink_ramfs_max_target -- ramfs stores SYMLINK_MAX-byte targets
 * ---------------------------------------------------------------------- */

static void test_symlink_ramfs_max_target(void) {
    char target[256];
    char buf[300];

    unlink("/tmp/sl_ramfs_long_link");

    for (int i = 0; i < 255; i++)
        target[i] = (char)('a' + (i % 26));
    target[255] = '\0';

    CHECK("symlink: symlink_ramfs_max_target create",
          symlink(target, "/tmp/sl_ramfs_long_link") == 0);

    memset(buf, 0, sizeof(buf));
    int n = readlink("/tmp/sl_ramfs_long_link", buf, sizeof(buf) - 1);
    CHECK("symlink: symlink_ramfs_max_target readlink length", n == 255);
    if (n > 0) {
        buf[n] = '\0';
        CHECK("symlink: symlink_ramfs_max_target target matches",
              strcmp(buf, target) == 0);
    }

    unlink("/tmp/sl_ramfs_long_link");
}

/* -----------------------------------------------------------------------
 * 17. symlink_empty_paths -- empty target is opaque, empty paths fail
 * ---------------------------------------------------------------------- */

static void test_symlink_empty_paths(void) {
    char buf[8];

    unlink("/mnt/sl_empty_target_link");
    errno = 0;
    int r = symlink("", "/mnt/sl_empty_target_link");
    CHECK("symlink: symlink_empty_paths empty target succeeds", r == 0);
    int n = readlink("/mnt/sl_empty_target_link", buf, sizeof(buf));
    CHECK("symlink: symlink_empty_paths empty target readlink zero", n == 0);
    unlink("/mnt/sl_empty_target_link");

    errno = 0;
    r = symlink("target", "");
    CHECK("symlink: symlink_empty_paths symlink returns -1", r == -1);
    CHECK("symlink: symlink_empty_paths symlink errno ENOENT", errno == ENOENT);

    errno = 0;
    n = readlink("", buf, sizeof(buf));
    CHECK("symlink: symlink_empty_paths readlink returns -1", n == -1);
    CHECK("symlink: symlink_empty_paths readlink errno ENOENT", errno == ENOENT);
}

/* -----------------------------------------------------------------------
 * symlink_posix_spec_edges -- Issue 8 edge cases for symlink/readlink/lchown
 * ---------------------------------------------------------------------- */

static void test_symlink_posix_spec_edges(void) {
    char b = 'x';
    struct stat st;

    unlink("/mnt/sl_zero_target_link");
    unlink("/mnt/sl_zero_target_file");
    CHECK("symlink: posix_spec_edges zero target setup",
          write_file("/mnt/sl_zero_target_file", "z", 1) == 0);
    CHECK("symlink: posix_spec_edges zero link setup",
          symlink("/mnt/sl_zero_target_file",
                  "/mnt/sl_zero_target_link") == 0);

    errno = 0;
    CHECK("symlink: posix_spec_edges readlink zero buffer succeeds",
          readlink("/mnt/sl_zero_target_link", &b, 0) == 0 &&
          errno == 0 && b == 'x');

    errno = 0;
    CHECK("symlink: posix_spec_edges readlink zero nonlink EINVAL",
          readlink("/mnt/sl_zero_target_file", &b, 0) == -1 &&
          errno == EINVAL && b == 'x');

    unlink("/mnt/sl_symlink_trailing_link");
    rmdir("/mnt/sl_symlink_trailing_dir");
    CHECK("symlink: posix_spec_edges trailing dir setup",
          mkdir("/mnt/sl_symlink_trailing_dir", 0755) == 0);
    errno = 0;
    CHECK("symlink: posix_spec_edges symlink trailing slash ENOTDIR",
          symlink("target", "/mnt/sl_symlink_trailing_dir/") == -1 &&
          errno == ENOTDIR);
    CHECK("symlink: posix_spec_edges trailing dir preserved",
          stat("/mnt/sl_symlink_trailing_dir", &st) == 0 &&
          S_ISDIR(st.st_mode));

    errno = 0;
    CHECK("symlink: posix_spec_edges lchown read-only EROFS",
          lchown("/bin/sh", 1, (gid_t)-1) == -1 && errno == EROFS);

    unlink("/mnt/sl_zero_target_link");
    unlink("/mnt/sl_zero_target_file");
    unlink("/mnt/sl_symlink_trailing_link");
    rmdir("/mnt/sl_symlink_trailing_dir");
}

/* -----------------------------------------------------------------------
 * 18. symlink_nofollow_open -- O_NOFOLLOW rejects final symlink
 * ---------------------------------------------------------------------- */

static void test_symlink_nofollow_open(void) {
    unlink("/mnt/sl_nofollow_link");
    unlink("/mnt/sl_nofollow_target");

    write_file("/mnt/sl_nofollow_target", "nofollow", 8);
    symlink("/mnt/sl_nofollow_target", "/mnt/sl_nofollow_link");

    errno = 0;
    int fd = open("/mnt/sl_nofollow_link", O_RDONLY | O_NOFOLLOW);
    CHECK("symlink: symlink_nofollow_open returns -1", fd == -1);
    CHECK("symlink: symlink_nofollow_open errno ELOOP", errno == ELOOP);
    if (fd >= 0) close(fd);

    unlink("/mnt/sl_nofollow_link");
    unlink("/mnt/sl_nofollow_target");
}

/* -----------------------------------------------------------------------
 * 19. symlink_overlong_target -- oversized target fails with ENAMETOOLONG
 * ---------------------------------------------------------------------- */

static void test_symlink_overlong_target(void) {
    char target[4200];
    memset(target, 'x', sizeof(target));
    target[sizeof(target) - 1] = '\0';

    unlink("/mnt/sl_overlong_link");
    errno = 0;
    int r = symlink(target, "/mnt/sl_overlong_link");
    CHECK("symlink: symlink_overlong_target returns -1", r == -1);
    CHECK("symlink: symlink_overlong_target errno ENAMETOOLONG",
          errno == ENAMETOOLONG);
    unlink("/mnt/sl_overlong_link");
}

static int find_dirent_type(const char *dirpath, const char *name, unsigned char *dtype) {
    DIR *dir = opendir(dirpath);
    if (!dir)
        return -1;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, name) == 0) {
            *dtype = de->d_type;
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

/* -----------------------------------------------------------------------
 * 20. symlink_linkat_follow -- AT_SYMLINK_FOLLOW links target file
 * ---------------------------------------------------------------------- */

static void test_linkat_symlink_follow(void) {
    const char *target = "/tmp/sl_linkat_target";
    const char *linkp = "/tmp/sl_linkat_sym";
    const char *hardp = "/tmp/sl_linkat_hard";

    unlink(hardp);
    unlink(linkp);
    unlink(target);

    CHECK("symlink: linkat_symlink_follow target setup",
          write_file(target, "lf", 2) == 0);
    CHECK("symlink: linkat_symlink_follow symlink setup",
          symlink(target, linkp) == 0);

    int r = linkat(AT_FDCWD, linkp, AT_FDCWD, hardp, AT_SYMLINK_FOLLOW);
    CHECK("symlink: linkat_symlink_follow returns 0", r == 0);

    struct stat st;
    int sr = lstat(hardp, &st);
    CHECK("symlink: linkat_symlink_follow hardlink is regular",
          sr == 0 && S_ISREG(st.st_mode));

    char buf[8];
    int n = read_file(hardp, buf, sizeof(buf));
    CHECK("symlink: linkat_symlink_follow content",
          n == 2 && memcmp(buf, "lf", 2) == 0);

    errno = 0;
    r = linkat(AT_FDCWD, target, AT_FDCWD, "/tmp/sl_linkat_bad", 0x8000);
    CHECK("symlink: linkat_unsupported_flags errno EINVAL",
          r == -1 && errno == EINVAL);

    unlink("/tmp/sl_linkat_bad");
    unlink(hardp);
    unlink(linkp);
    unlink(target);
}

/* -----------------------------------------------------------------------
 * 21. symlink_dirent_metadata -- filesystem readdir reports DT_LNK
 * ---------------------------------------------------------------------- */

static void test_symlink_dirent_metadata(void) {
    unsigned char dtype = 0;

    unlink("/mnt/sl_dtype_link");
    CHECK("symlink: dirent_metadata ext2 create",
          symlink("/mnt/sl_no_such_target", "/mnt/sl_dtype_link") == 0);
    CHECK("symlink: dirent_metadata ext2 DT_LNK",
          find_dirent_type("/mnt", "sl_dtype_link", &dtype) == 0 &&
          dtype == DT_LNK);
    unlink("/mnt/sl_dtype_link");

    struct stat st;
    int sr = lstat("/bin/sh", &st);
    CHECK("symlink: dirent_metadata tarfs lstat link",
          sr == 0 && S_ISLNK(st.st_mode) && st.st_size == 5);
    CHECK("symlink: dirent_metadata tarfs DT_LNK",
          find_dirent_type("/bin", "sh", &dtype) == 0 && dtype == DT_LNK);
}

/* -----------------------------------------------------------------------
 * 22. symlink_rename_trailing -- rename rejects final symlink plus slash
 * ---------------------------------------------------------------------- */

static void test_rename_trailing_symlink(void) {
    struct stat st;

    unlink("/mnt/sl_rename_src_link");
    rmdir("/mnt/sl_rename_src_target");
    rmdir("/mnt/sl_rename_src_new");
    unlink("/mnt/sl_rename_dst_file");
    unlink("/mnt/sl_rename_dst_link");
    rmdir("/mnt/sl_rename_dst_target");

    CHECK("symlink: rename_trailing source dir setup",
          mkdir("/mnt/sl_rename_src_target", 0755) == 0);
    CHECK("symlink: rename_trailing source link setup",
          symlink("/mnt/sl_rename_src_target",
                  "/mnt/sl_rename_src_link") == 0);

    errno = 0;
    int r = rename("/mnt/sl_rename_src_link/", "/mnt/sl_rename_src_new");
    CHECK("symlink: rename_trailing source link slash ENOTDIR",
          r == -1 && errno == ENOTDIR);
    CHECK("symlink: rename_trailing source target preserved",
          stat("/mnt/sl_rename_src_target", &st) == 0 &&
          S_ISDIR(st.st_mode));
    CHECK("symlink: rename_trailing source link preserved",
          lstat("/mnt/sl_rename_src_link", &st) == 0 &&
          S_ISLNK(st.st_mode));

    CHECK("symlink: rename_trailing destination file setup",
          write_file("/mnt/sl_rename_dst_file", "r", 1) == 0);
    CHECK("symlink: rename_trailing destination dir setup",
          mkdir("/mnt/sl_rename_dst_target", 0755) == 0);
    CHECK("symlink: rename_trailing destination link setup",
          symlink("/mnt/sl_rename_dst_target",
                  "/mnt/sl_rename_dst_link") == 0);

    errno = 0;
    r = rename("/mnt/sl_rename_dst_file", "/mnt/sl_rename_dst_link/");
    CHECK("symlink: rename_trailing destination link slash ENOTDIR",
          r == -1 && errno == ENOTDIR);
    CHECK("symlink: rename_trailing source file preserved",
          stat("/mnt/sl_rename_dst_file", &st) == 0 &&
          S_ISREG(st.st_mode));
    CHECK("symlink: rename_trailing destination link preserved",
          lstat("/mnt/sl_rename_dst_link", &st) == 0 &&
          S_ISLNK(st.st_mode));

    unlink("/mnt/sl_rename_src_link");
    rmdir("/mnt/sl_rename_src_target");
    rmdir("/mnt/sl_rename_src_new");
    unlink("/mnt/sl_rename_dst_file");
    unlink("/mnt/sl_rename_dst_link");
    rmdir("/mnt/sl_rename_dst_target");
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
    test_readlink_nonlink_variants();
    test_readlinkat_nonlink_variants();
    test_stat_follows_symlink();
    test_lstat_no_follow();
    test_symlink_relative_target();
    test_realpath_relative_symlink_parent();
    test_realpath_errno_edges();
    test_realpath_complex_edges();
    test_symlink_unlink();
    test_symlink_dangling();
    test_symlink_dangling_create();
    test_readlink_intermediate_dir_symlink();
    test_symlinkat_parent_symlink();
    test_symlink_long_target();
    test_symlink_ramfs_max_target();
    test_symlink_empty_paths();
    test_symlink_posix_spec_edges();
    test_symlink_nofollow_open();
    test_symlink_overlong_target();
    test_linkat_symlink_follow();
    test_symlink_dirent_metadata();
    test_rename_trailing_symlink();

    printf("=== symlinktest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
