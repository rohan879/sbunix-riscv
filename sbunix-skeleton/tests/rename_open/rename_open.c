/*
 * rename_open.c -- Verify rename semantics:
 *   1. rename-to-self returns 0 (no-op, POSIX requirement)
 *   2. renaming a file while an fd is open does not break the fd
 *
 * These tests run on /tmp (ramfs) since it is writable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

int main(void) {
  /* --- Test 1: rename-to-self returns 0 --- */
  {
    /* Create a file on /tmp (ramfs, writable) */
    int fd = open("/tmp/rename_self_test", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
      printf("[FAIL] rename_over_open (cannot create /tmp/rename_self_test)\n");
      return 1;
    }
    write(fd, "hello", 5);
    close(fd);

    /* Rename to self -- should be a no-op and return 0 */
    int ret = rename("/tmp/rename_self_test", "/tmp/rename_self_test");
    CHECK("rename_self_noop", ret == 0);

    /* Clean up */
    unlink("/tmp/rename_self_test");
  }

  /* --- Test 2: rename while fd is open --- */
  {
    const char *oldname = "/tmp/rename_open_old";
    const char *newname = "/tmp/rename_open_new";
    const char *data = "test data for rename";

    /* Create and write to old file */
    int fd = open(oldname, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
      printf("[FAIL] rename_over_open (cannot create %s)\n", oldname);
      return 1;
    }
    write(fd, data, strlen(data));

    /* Seek back to start so we can read after rename */
    lseek(fd, 0, 0 /* SEEK_SET */);

    /* Rename while fd is still open */
    int ret = rename(oldname, newname);
    CHECK("rename_while_open", ret == 0);

    /* Verify fd still works -- read the data back */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(fd, buf, sizeof(buf) - 1);
    /* The fd should still be readable and contain our data */
    CHECK("rename_over_open",
          nr == (ssize_t)strlen(data) &&
          memcmp(buf, data, strlen(data)) == 0);

    close(fd);

    /* Clean up */
    unlink(newname);
  }

  return g_fail > 0 ? 1 : 0;
}
