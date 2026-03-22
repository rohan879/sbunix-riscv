/*
 * fstest.c -- On-disk ext2 filesystem tests (mounted at /mnt)
 *
 * Exercises create, read, write, unlink, mkdir, rmdir, lseek, append,
 * truncate, stat, and readdir on the ext2 filesystem.
 *
 * Run from the shell:  fstest
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

/* -----------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. create_write_read — basic file I/O round-trip
 * ---------------------------------------------------------------------- */

static void test_create_write_read(void) {
    const char *path = "/mnt/testfile";
    const char *msg  = "hello world";

    /* cleanup from prior run */
    unlink(path);

    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    CHECK("cwr: open O_CREAT|O_WRONLY", fd >= 0);
    if (fd < 0) return;

    ssize_t nw = write(fd, msg, strlen(msg));
    CHECK("cwr: write 11 bytes", nw == (ssize_t)strlen(msg));
    close(fd);

    fd = open(path, O_RDONLY);
    CHECK("cwr: reopen O_RDONLY", fd >= 0);
    if (fd < 0) { unlink(path); return; }

    char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(fd, buf, sizeof(buf));
    CHECK("cwr: read back matches", nr == (ssize_t)strlen(msg) &&
          memcmp(buf, msg, strlen(msg)) == 0);
    close(fd);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 2. mkdir_stat — create a directory and verify with stat
 * ---------------------------------------------------------------------- */

static void test_mkdir_stat(void) {
    const char *path = "/mnt/testdir";

    /* cleanup from prior run */
    rmdir(path);

    int r = mkdir(path, 0755);
    CHECK("mkdir: create /mnt/testdir", r == 0);

    struct stat st;
    r = stat(path, &st);
    CHECK("mkdir: stat returns 0", r == 0);
    CHECK("mkdir: S_ISDIR", S_ISDIR(st.st_mode));

    rmdir(path);
}

/* -----------------------------------------------------------------------
 * 3. unlink_enoent — unlink a file then verify it is gone
 * ---------------------------------------------------------------------- */

static void test_unlink_enoent(void) {
    const char *path = "/mnt/unlinkme";

    /* cleanup from prior run */
    unlink(path);

    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    CHECK("unlink: create file", fd >= 0);
    if (fd >= 0) close(fd);

    int r = unlink(path);
    CHECK("unlink: unlink returns 0", r == 0);

    fd = open(path, O_RDONLY);
    CHECK("unlink: open after unlink returns -1", fd == -1);
    if (fd >= 0) close(fd);
}

/* -----------------------------------------------------------------------
 * 4. multiblock_write — write and verify 8192 bytes
 * ---------------------------------------------------------------------- */

static void test_multiblock_write(void) {
    const char *path = "/mnt/bigfile";
    const int   size = 8192;

    /* cleanup from prior run */
    unlink(path);

    unsigned char wbuf[8192];
    for (int i = 0; i < size; i++)
        wbuf[i] = (unsigned char)(i % 251);

    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    CHECK("multi: open for write", fd >= 0);
    if (fd < 0) return;

    ssize_t nw = write(fd, wbuf, size);
    CHECK("multi: wrote 8192 bytes", nw == size);
    close(fd);

    fd = open(path, O_RDONLY);
    CHECK("multi: reopen for read", fd >= 0);
    if (fd < 0) { unlink(path); return; }

    unsigned char rbuf[8192];
    memset(rbuf, 0, sizeof(rbuf));

    /* read back in a loop in case read() returns a short count */
    int total = 0;
    while (total < size) {
        ssize_t nr = read(fd, rbuf + total, size - total);
        if (nr <= 0) break;
        total += (int)nr;
    }
    CHECK("multi: read back 8192 bytes", total == size);

    int match = 1;
    for (int i = 0; i < size; i++) {
        if (rbuf[i] != (unsigned char)(i % 251)) {
            match = 0;
            break;
        }
    }
    CHECK("multi: data integrity", match);
    close(fd);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 5. lseek_read — seek to offset and read partial data
 * ---------------------------------------------------------------------- */

static void test_lseek_read(void) {
    const char *path = "/mnt/seekfile";
    const char *data = "abcdefghij";

    /* cleanup from prior run */
    unlink(path);

    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    CHECK("seek: create file", fd >= 0);
    if (fd < 0) return;

    write(fd, data, 10);
    close(fd);

    fd = open(path, O_RDONLY);
    CHECK("seek: reopen O_RDONLY", fd >= 0);
    if (fd < 0) { unlink(path); return; }

    long pos = lseek(fd, 5, SEEK_SET);
    CHECK("seek: lseek to offset 5", pos == 5);

    char buf[6];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(fd, buf, 5);
    CHECK("seek: read 'fghij'", nr == 5 && memcmp(buf, "fghij", 5) == 0);
    close(fd);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 6. append_mode — O_APPEND writes at end
 * ---------------------------------------------------------------------- */

static void test_append_mode(void) {
    const char *path = "/mnt/appendfile";

    /* cleanup from prior run */
    unlink(path);

    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    CHECK("append: create file", fd >= 0);
    if (fd < 0) return;

    write(fd, "first", 5);
    close(fd);

    fd = open(path, O_WRONLY | O_APPEND);
    CHECK("append: reopen O_APPEND", fd >= 0);
    if (fd < 0) { unlink(path); return; }

    write(fd, "second", 6);
    close(fd);

    fd = open(path, O_RDONLY);
    CHECK("append: reopen O_RDONLY", fd >= 0);
    if (fd < 0) { unlink(path); return; }

    char buf[16];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(fd, buf, sizeof(buf));
    CHECK("append: content is 'firstsecond'",
          nr == 11 && memcmp(buf, "firstsecond", 11) == 0);
    close(fd);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 7. stat_size — verify st_size after write
 * ---------------------------------------------------------------------- */

static void test_stat_size(void) {
    const char *path = "/mnt/sizefile";

    /* cleanup from prior run */
    unlink(path);

    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    CHECK("size: create file", fd >= 0);
    if (fd < 0) return;

    char buf[100];
    memset(buf, 'Z', 100);
    write(fd, buf, 100);
    close(fd);

    struct stat st;
    int r = stat(path, &st);
    CHECK("size: stat returns 0", r == 0);
    CHECK("size: st_size == 100", st.st_size == 100);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 8. open_enoent — open a nonexistent file
 * ---------------------------------------------------------------------- */

static void test_open_enoent(void) {
    int fd = open("/mnt/nonexistent", O_RDONLY);
    CHECK("enoent: open returns -1", fd == -1);
    if (fd >= 0) close(fd);
}

/* -----------------------------------------------------------------------
 * 9. truncate_file — O_TRUNC resets file to zero length
 * ---------------------------------------------------------------------- */

static void test_truncate_file(void) {
    const char *path = "/mnt/truncfile";

    /* cleanup from prior run */
    unlink(path);

    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    CHECK("trunc: create file", fd >= 0);
    if (fd < 0) return;

    write(fd, "some data", 9);
    close(fd);

    fd = open(path, O_WRONLY | O_TRUNC);
    CHECK("trunc: reopen O_TRUNC", fd >= 0);
    if (fd >= 0) close(fd);

    struct stat st;
    int r = stat(path, &st);
    CHECK("trunc: stat returns 0", r == 0);
    CHECK("trunc: st_size == 0", st.st_size == 0);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 10. nested_mkdir — create nested directories
 * ---------------------------------------------------------------------- */

static void test_nested_mkdir(void) {
    const char *outer = "/mnt/adir";
    const char *inner = "/mnt/adir/bdir";

    /* cleanup from prior run (inner first) */
    rmdir(inner);
    rmdir(outer);

    int r1 = mkdir(outer, 0755);
    CHECK("nest: mkdir /mnt/adir", r1 == 0);

    int r2 = mkdir(inner, 0755);
    CHECK("nest: mkdir /mnt/adir/bdir", r2 == 0);

    struct stat st1, st2;
    int s1 = stat(outer, &st1);
    int s2 = stat(inner, &st2);
    CHECK("nest: adir is directory",  s1 == 0 && S_ISDIR(st1.st_mode));
    CHECK("nest: bdir is directory",  s2 == 0 && S_ISDIR(st2.st_mode));

    rmdir(inner);
    rmdir(outer);
}

/* -----------------------------------------------------------------------
 * 11. readdir_finds_file — create a file and find it with readdir
 * ---------------------------------------------------------------------- */

static void test_readdir_finds_file(void) {
    const char *path = "/mnt/findme";

    /* cleanup from prior run */
    unlink(path);

    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    CHECK("readdir: create /mnt/findme", fd >= 0);
    if (fd >= 0) close(fd);

    DIR *d = opendir("/mnt");
    CHECK("readdir: opendir /mnt", d != (void *)0);

    int found = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != (void *)0) {
            if (strcmp(de->d_name, "findme") == 0) {
                found = 1;
                break;
            }
        }
        closedir(d);
    }
    CHECK("readdir: found 'findme'", found);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 12. mkdir_exists_error — mkdir on existing directory returns EEXIST
 * ---------------------------------------------------------------------- */

static void test_mkdir_exists_error(void) {
    const char *path = "/mnt/existdir";

    /* cleanup from prior run */
    rmdir(path);

    int r = mkdir(path, 0755);
    CHECK("mkdir_exists: first mkdir succeeds", r == 0);

    errno = 0;
    int r2 = mkdir(path, 0755);
    CHECK("mkdir_exists: second mkdir returns -1", r2 == -1);
    CHECK("mkdir_exists: errno is EEXIST", errno == EEXIST);

    rmdir(path);
}

/* -----------------------------------------------------------------------
 * 13. unlink_nonexistent — unlink on nonexistent path returns ENOENT
 * ---------------------------------------------------------------------- */

static void test_unlink_nonexistent(void) {
    const char *path = "/mnt/no_such_file_here";

    /* make sure it does not exist */
    unlink(path);

    errno = 0;
    int r = unlink(path);
    CHECK("unlink_nonexistent: returns -1", r == -1);
    CHECK("unlink_nonexistent: errno is ENOENT", errno == ENOENT);
}

/* -----------------------------------------------------------------------
 * 14. rename_file — rename a file, old path gone, new path has content
 * ---------------------------------------------------------------------- */

static void test_rename_file(void) {
    const char *oldpath = "/mnt/rename_old";
    const char *newpath = "/mnt/rename_new";
    const char *data    = "rename_test_data";

    /* cleanup from prior run */
    unlink(oldpath);
    unlink(newpath);

    int fd = open(oldpath, O_CREAT | O_WRONLY, 0644);
    CHECK("rename: create old file", fd >= 0);
    if (fd < 0) return;

    write(fd, data, strlen(data));
    close(fd);

    int r = rename(oldpath, newpath);
    CHECK("rename: rename returns 0", r == 0);

    /* Old path should be gone */
    fd = open(oldpath, O_RDONLY);
    CHECK("rename: old path is gone", fd == -1);
    if (fd >= 0) close(fd);

    /* New path should have the data */
    fd = open(newpath, O_RDONLY);
    CHECK("rename: new path exists", fd >= 0);
    if (fd < 0) { unlink(newpath); return; }

    char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(fd, buf, sizeof(buf));
    CHECK("rename: data preserved",
          nr == (ssize_t)strlen(data) &&
          memcmp(buf, data, strlen(data)) == 0);
    close(fd);

    unlink(newpath);
}

/* -----------------------------------------------------------------------
 * 15. readdir_entries — create 3 files in a dir, readdir finds all 3
 * ---------------------------------------------------------------------- */

static void test_readdir_entries(void) {
    const char *dir  = "/mnt/rddir";
    const char *f1   = "/mnt/rddir/file_a";
    const char *f2   = "/mnt/rddir/file_b";
    const char *f3   = "/mnt/rddir/file_c";

    /* cleanup from prior run */
    unlink(f1); unlink(f2); unlink(f3);
    rmdir(dir);

    int r = mkdir(dir, 0755);
    CHECK("readdir3: mkdir", r == 0);
    if (r != 0) return;

    /* Create three files */
    int fd;
    fd = open(f1, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    fd = open(f2, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    fd = open(f3, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);

    DIR *d = opendir(dir);
    CHECK("readdir3: opendir", d != (void *)0);
    if (!d) goto readdir3_cleanup;

    int found_a = 0, found_b = 0, found_c = 0;
    struct dirent *de;
    while ((de = readdir(d)) != (void *)0) {
        if (strcmp(de->d_name, "file_a") == 0) found_a = 1;
        if (strcmp(de->d_name, "file_b") == 0) found_b = 1;
        if (strcmp(de->d_name, "file_c") == 0) found_c = 1;
    }
    closedir(d);

    CHECK("readdir3: found file_a", found_a);
    CHECK("readdir3: found file_b", found_b);
    CHECK("readdir3: found file_c", found_c);

readdir3_cleanup:
    unlink(f1); unlink(f2); unlink(f3);
    rmdir(dir);
}

/* -----------------------------------------------------------------------
 * 16. lseek_past_end — seek past end, write, file has gap
 * ---------------------------------------------------------------------- */

static void test_lseek_past_end(void) {
    const char *path = "/mnt/seekpast";

    /* cleanup from prior run */
    unlink(path);

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    CHECK("seekpast: create file", fd >= 0);
    if (fd < 0) return;

    /* Write 4 bytes at offset 0 */
    write(fd, "ABCD", 4);

    /* Seek past the end to offset 100 */
    off_t pos = lseek(fd, 100, SEEK_SET);
    CHECK("seekpast: lseek to 100", pos == 100);

    /* Write 4 bytes at offset 100 */
    write(fd, "EFGH", 4);
    close(fd);

    /* Verify file size is at least 104 */
    struct stat st;
    int r = stat(path, &st);
    CHECK("seekpast: stat returns 0", r == 0);
    CHECK("seekpast: st_size >= 104", st.st_size >= 104);

    /* Verify the gap region (offsets 4..99) reads as zeros */
    fd = open(path, O_RDONLY);
    CHECK("seekpast: reopen for read", fd >= 0);
    if (fd < 0) { unlink(path); return; }

    lseek(fd, 4, SEEK_SET);
    char gap[96];
    memset(gap, 0xFF, sizeof(gap));
    ssize_t nr = read(fd, gap, 96);
    int zeros = 1;
    int i;
    for (i = 0; i < (int)nr; i++) {
        if (gap[i] != 0) {
            zeros = 0;
            break;
        }
    }
    CHECK("seekpast: gap is zero-filled", nr == 96 && zeros);

    /* Verify trailing data */
    char tail[4];
    memset(tail, 0, sizeof(tail));
    nr = read(fd, tail, 4);
    CHECK("seekpast: trailing data is EFGH",
          nr == 4 && memcmp(tail, "EFGH", 4) == 0);
    close(fd);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 17. nested_directories — mkdir nested, create file inside, read back
 * ---------------------------------------------------------------------- */

static void test_nested_dir_file(void) {
    const char *outer = "/mnt/ndir1";
    const char *inner = "/mnt/ndir1/ndir2";
    const char *fpath = "/mnt/ndir1/ndir2/nested.txt";
    const char *data  = "nested_content_ok";

    /* cleanup from prior run */
    unlink(fpath);
    rmdir(inner);
    rmdir(outer);

    int r1 = mkdir(outer, 0755);
    CHECK("nested_file: mkdir outer", r1 == 0);

    int r2 = mkdir(inner, 0755);
    CHECK("nested_file: mkdir inner", r2 == 0);

    int fd = open(fpath, O_CREAT | O_WRONLY, 0644);
    CHECK("nested_file: create file in nested dir", fd >= 0);
    if (fd < 0) goto nested_cleanup;

    write(fd, data, strlen(data));
    close(fd);

    fd = open(fpath, O_RDONLY);
    CHECK("nested_file: reopen for read", fd >= 0);
    if (fd < 0) goto nested_cleanup;

    char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(fd, buf, sizeof(buf));
    CHECK("nested_file: data matches",
          nr == (ssize_t)strlen(data) &&
          memcmp(buf, data, strlen(data)) == 0);
    close(fd);

nested_cleanup:
    unlink(fpath);
    rmdir(inner);
    rmdir(outer);
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== fstest: on-disk filesystem ===\n");

    test_create_write_read();
    test_mkdir_stat();
    test_unlink_enoent();
    test_multiblock_write();
    test_lseek_read();
    test_append_mode();
    test_stat_size();
    test_open_enoent();
    test_truncate_file();
    test_nested_mkdir();
    test_readdir_finds_file();
    test_mkdir_exists_error();
    test_unlink_nonexistent();
    test_rename_file();
    test_readdir_entries();
    test_lseek_past_end();
    test_nested_dir_file();

    printf("=== fstest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
