#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BLOCK 512
#define BLOCKS 8

static void fill_pattern(unsigned char *buf, int iter, int block) {
  for (int i = 0; i < BLOCK; i++)
    buf[i] = (unsigned char)((iter * 131 + block * 17 + i) & 0xff);
}

static int run_one(int iter) {
  const char *path = "/mnt/virtiostress.tmp";
  unsigned char out[BLOCK];
  unsigned char in[BLOCK];

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0)
    return 0;

  for (int b = 0; b < BLOCKS; b++) {
    fill_pattern(out, iter, b);
    if (write(fd, out, BLOCK) != BLOCK)
      return 0;
  }
  fsync(fd);
  close(fd);

  fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;

  for (int b = 0; b < BLOCKS; b++) {
    fill_pattern(out, iter, b);
    if (read(fd, in, BLOCK) != BLOCK)
      return 0;
    if (memcmp(out, in, BLOCK) != 0)
      return 0;
  }

  close(fd);
  unlink(path);
  return 1;
}

int main(int argc, char **argv) {
  int iterations = 256;
  if (argc > 1 && strcmp(argv[1], "--long") == 0)
    iterations = 1024;

  printf("=== virtiostress ===\n");
  for (int i = 0; i < iterations; i++) {
    if (!run_one(i)) {
      printf("[FAIL] virtio stress iteration=%d\n", i);
      return 1;
    }
  }
  printf("[PASS] virtio stress iterations=%d\n", iterations);
  return 0;
}
