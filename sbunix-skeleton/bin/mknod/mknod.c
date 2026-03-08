#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
  if (argc != 5) {
    dprintf(2, "usage: mknod <path> <c|b> <major> <minor>\n");
    return 1;
  }

  const char *path = argv[1];
  const char *type = argv[2];
  int maj = atoi(argv[3]);
  int min = atoi(argv[4]);

  mode_t mode;
  if (strcmp(type, "c") == 0)
    mode = S_IFCHR | 0666;
  else if (strcmp(type, "b") == 0)
    mode = S_IFBLK | 0666;
  else {
    dprintf(2, "mknod: type must be 'c' or 'b'\n");
    return 1;
  }

  if (mknod(path, mode, makedev(maj, min)) < 0) {
    dprintf(2, "mknod: cannot create '%s': %s\n", path, strerror(errno));
    return 1;
  }

  return 0;
}
