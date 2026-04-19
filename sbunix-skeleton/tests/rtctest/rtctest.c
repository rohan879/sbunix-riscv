#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_pass;
static int g_fail;

#define CHECK(label, cond) do { \
  if (cond) { printf("[PASS] rtc: %s\n", label); g_pass++; } \
  else { printf("[FAIL] rtc: %s\n", label); g_fail++; } \
} while (0)

static int read_attr(const char *path, char *buf, int buflen) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int n = read(fd, buf, buflen - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return n;
}

static void trim_newline(char *s) {
  int n = (int)strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
    s[--n] = '\0';
}

int main(void) {
  char buf[128];

  printf("=== rtctest: sysfs RTC discovery ===\n");

  CHECK("address",
        read_attr("/sys/devices/rtc0/address", buf, sizeof(buf)) > 0 &&
        (trim_newline(buf), strcmp(buf, "0x101000") == 0));

  CHECK("size",
        read_attr("/sys/devices/rtc0/size", buf, sizeof(buf)) > 0 &&
        (trim_newline(buf), strcmp(buf, "0x1000") == 0));

  CHECK("irq",
        read_attr("/sys/devices/rtc0/irq", buf, sizeof(buf)) > 0 &&
        (trim_newline(buf), strcmp(buf, "11") == 0));

  CHECK("compatible",
        read_attr("/sys/devices/rtc0/compatible", buf, sizeof(buf)) > 0 &&
        (trim_newline(buf), strcmp(buf, "google,goldfish-rtc") == 0));

  printf("=== rtctest: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
