#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_pass;
static int g_fail;

static void check(const char *name, int ok) {
  if (ok) {
    g_pass++;
    printf("[PASS] %s\n", name);
  } else {
    g_fail++;
    printf("[FAIL] %s errno=%d\n", name, errno);
  }
}

static void cleanup(void) {
  unlink("/tmp/smeta_link");
  unlink("/mnt/smeta_link");
}

static void exercise_one(const char *label, const char *path) {
  struct stat st;
  struct timespec ts[2];

  unlink(path);
  check(label, symlink("smeta-target", path) == 0);
  check("symlink_metadata_edges: lstat sees symlink",
        lstat(path, &st) == 0 && S_ISLNK(st.st_mode) &&
        st.st_uid == 11 && st.st_gid == 22);

  check("symlink_metadata_edges: fchownat nofollow updates owner",
        fchownat(AT_FDCWD, path, 33, 44, AT_SYMLINK_NOFOLLOW) == 0);
  check("symlink_metadata_edges: owner changed on link",
        lstat(path, &st) == 0 && S_ISLNK(st.st_mode) &&
        st.st_uid == 33 && st.st_gid == 44);

  ts[0].tv_sec = 111;
  ts[0].tv_nsec = 0;
  ts[1].tv_sec = 222;
  ts[1].tv_nsec = 0;
  check("symlink_metadata_edges: utimensat nofollow updates link",
        utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW) == 0);
  check("symlink_metadata_edges: link mtime changed",
        lstat(path, &st) == 0 && S_ISLNK(st.st_mode) &&
        st.st_mtime == 222);

  check("symlink_metadata_edges: fchmodat nofollow validates",
        fchmodat(AT_FDCWD, path, 0600, AT_SYMLINK_NOFOLLOW) == 0);
  check("symlink_metadata_edges: chmod leaves object a link",
        lstat(path, &st) == 0 && S_ISLNK(st.st_mode));

  unlink(path);
}

int main(void) {
  printf("=== symlink_metadata_edges ===\n");

  cleanup();
  check("symlink_metadata_edges: setuid", setuid(11) == 0);
  check("symlink_metadata_edges: setgid", setgid(22) == 0);
  exercise_one("symlink_metadata_edges: ramfs symlink create",
               "/tmp/smeta_link");
  exercise_one("symlink_metadata_edges: ext2 symlink create",
               "/mnt/smeta_link");
  cleanup();

  printf("=== symlink_metadata_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
