#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
  if (argc != 3) {
    dprintf(2, "usage: cp <src> <dst>\n");
    return 1;
  }

  int src = open(argv[1], O_RDONLY);
  if (src < 0) {
    dprintf(2, "cp: cannot open '%s': %s\n", argv[1], strerror(errno));
    return 1;
  }

  int dst = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
  if (dst < 0) {
    dprintf(2, "cp: cannot create '%s': %s\n", argv[2], strerror(errno));
    close(src);
    return 1;
  }

  char buf[4096];
  ssize_t n;
  while ((n = read(src, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(dst, buf + off, (size_t)(n - off));
      if (w < 0) {
        dprintf(2, "cp: write error: %s\n", strerror(errno));
        close(src);
        close(dst);
        return 1;
      }
      off += w;
    }
  }

  if (n < 0) {
    dprintf(2, "cp: read error: %s\n", strerror(errno));
    close(src);
    close(dst);
    return 1;
  }

  close(src);
  close(dst);
  return 0;
}
