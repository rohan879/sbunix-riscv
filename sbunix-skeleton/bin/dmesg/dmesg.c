#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(void) {
  int fd = open("/proc/dmesg", O_RDONLY);
  if (fd < 0) {
    dprintf(2, "dmesg: cannot open /proc/dmesg\n");
    return 1;
  }
  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    write(STDOUT_FILENO, buf, (size_t)n);
  close(fd);
  return 0;
}
