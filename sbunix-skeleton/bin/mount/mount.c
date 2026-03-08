#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char *argv[]) {
  if (argc > 1) {
    dprintf(2, "usage: mount (no arguments — displays mount table)\n");
    return 1;
  }
  (void)argv;

  int fd = open("/proc/mounts", O_RDONLY);
  if (fd < 0) {
    dprintf(2, "mount: cannot open /proc/mounts\n");
    return 1;
  }
  char buf[512];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    write(STDOUT_FILENO, buf, (size_t)n);
  close(fd);
  return 0;
}
