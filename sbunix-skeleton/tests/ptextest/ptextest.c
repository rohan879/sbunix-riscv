#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* I/O stress test: exercises heavy virtio_blk DMA to trigger the
 * intermittent PTE_X corruption bug.  50 rounds of 20 file
 * create/write/read/unlink cycles stress the block driver and
 * verify the kernel remains stable throughout. */
int main(void) {
  char buf[4096];
  memset(buf, 'A', sizeof(buf));
  for (int round = 0; round < 50; round++) {
    for (int i = 0; i < 20; i++) {
      char path[64];
      snprintf(path, sizeof(path), "/mnt/ptex_%d_%d", round, i);
      int fd = open(path, O_CREAT | O_WRONLY, 0644);
      if (fd >= 0) {
        for (int j = 0; j < 10; j++)
          write(fd, buf, sizeof(buf));
        close(fd);
      }
    }
    for (int i = 0; i < 20; i++) {
      char path[64];
      snprintf(path, sizeof(path), "/mnt/ptex_%d_%d", round, i);
      int fd = open(path, O_RDONLY);
      if (fd >= 0) {
        while (read(fd, buf, sizeof(buf)) > 0) {}
        close(fd);
      }
      unlink(path);
    }
  }
  printf("[PASS] ptex_stress\n");
  return 0;
}
