#include <sys/times.h>
#include <syscall.h>
#include <errno.h>

clock_t times(struct tms *buf) {
  return (clock_t)__syscall_ret(syscall(SYS_times, buf));
}
