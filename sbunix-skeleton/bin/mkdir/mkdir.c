#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static int mkpath(const char *path, mode_t mode) {
  char buf[256];
  int len = strlen(path);
  if (len >= (int)sizeof(buf)) {
    dprintf(2, "mkdir: path too long\n");
    return -1;
  }
  strcpy(buf, path);

  for (int i = 1; i <= len; i++) {
    if (buf[i] == '/' || buf[i] == '\0') {
      char saved = buf[i];
      buf[i] = '\0';
      if (mkdir(buf, mode) < 0 && errno != EEXIST) {
        /* Check if it already exists as a directory */
        struct stat st;
        if (stat(buf, &st) < 0 || !S_ISDIR(st.st_mode)) {
          dprintf(2, "mkdir: cannot create '%s': %s\n", buf, strerror(errno));
          return -1;
        }
      }
      buf[i] = saved;
    }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int pflag = 0;
  int start = 1;
  int ret = 0;

  if (argc > 1 && strcmp(argv[1], "-p") == 0) {
    pflag = 1;
    start = 2;
  }

  if (start >= argc) {
    dprintf(2, "usage: mkdir [-p] <dir> ...\n");
    return 1;
  }

  for (int i = start; i < argc; i++) {
    if (pflag) {
      if (mkpath(argv[i], 0755) < 0)
        ret = 1;
    } else {
      if (mkdir(argv[i], 0755) < 0) {
        dprintf(2, "mkdir: cannot create '%s': %s\n", argv[i], strerror(errno));
        ret = 1;
      }
    }
  }
  return ret;
}
