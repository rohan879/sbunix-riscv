#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>

static int rflag, fflag;

static int rm_recursive(const char *path) {
  DIR *d = opendir(path);
  if (!d) {
    if (unlink(path) < 0) {
      if (!fflag || errno != ENOENT)
        dprintf(2, "rm: cannot remove '%s': %s\n", path, strerror(errno));
      return (fflag && errno == ENOENT) ? 0 : 1;
    }
    return 0;
  }

  int ret = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;

    char child[256];
    int plen = strlen(path);
    int nlen = strlen(ent->d_name);
    if (plen + 1 + nlen >= (int)sizeof(child)) {
      dprintf(2, "rm: path too long\n");
      ret = 1;
      continue;
    }
    strcpy(child, path);
    child[plen] = '/';
    strcpy(child + plen + 1, ent->d_name);

    if (ent->d_type == DT_DIR) {
      if (rm_recursive(child) != 0)
        ret = 1;
    } else {
      if (unlink(child) < 0) {
        dprintf(2, "rm: cannot remove '%s': %s\n", child, strerror(errno));
        ret = 1;
      }
    }
  }
  closedir(d);

  if (rmdir(path) < 0) {
    dprintf(2, "rm: cannot remove '%s': %s\n", path, strerror(errno));
    ret = 1;
  }
  return ret;
}

int main(int argc, char *argv[]) {
  rflag = 0;
  fflag = 0;
  int start = 1;

  /* Parse flags */
  for (int i = 1; i < argc && argv[i][0] == '-'; i++) {
    for (int j = 1; argv[i][j]; j++) {
      if (argv[i][j] == 'r' || argv[i][j] == 'R')
        rflag = 1;
      else if (argv[i][j] == 'f')
        fflag = 1;
      else {
        dprintf(2, "rm: unknown option '-%c'\n", argv[i][j]);
        return 1;
      }
    }
    start = i + 1;
  }

  if (start >= argc) {
    if (!fflag) {
      dprintf(2, "usage: rm [-rf] <file> ...\n");
      return 1;
    }
    return 0;
  }

  int ret = 0;
  for (int i = start; i < argc; i++) {
    if (rflag) {
      if (rm_recursive(argv[i]) != 0)
        ret = 1;
    } else {
      if (unlink(argv[i]) < 0) {
        if (!fflag || errno != ENOENT) {
          dprintf(2, "rm: cannot remove '%s': %s\n", argv[i], strerror(errno));
          ret = 1;
        }
      }
    }
  }
  return ret;
}
