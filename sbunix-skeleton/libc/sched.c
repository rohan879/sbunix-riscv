#include <sched.h>

#include <errno.h>
#include <unistd.h>

int sched_get_priority_max(int policy) {
  if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

int sched_get_priority_min(int policy) {
  if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

int sched_getparam(pid_t pid, struct sched_param *param) {
  if (!param) {
    errno = EINVAL;
    return -1;
  }
  if (pid < 0) {
    errno = ESRCH;
    return -1;
  }
  param->sched_priority = 0;
  return 0;
}

int sched_setparam(pid_t pid, const struct sched_param *param) {
  (void)param;
  if (pid < 0) {
    errno = ESRCH;
    return -1;
  }
  return 0;
}

int sched_getscheduler(pid_t pid) {
  if (pid < 0) {
    errno = ESRCH;
    return -1;
  }
  return SCHED_OTHER;
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param) {
  (void)param;
  if (pid < 0) {
    errno = ESRCH;
    return -1;
  }
  if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR) {
    errno = EINVAL;
    return -1;
  }
  return policy;
}

int sched_rr_get_interval(pid_t pid, struct timespec *interval) {
  if (pid < 0) {
    errno = ESRCH;
    return -1;
  }
  if (!interval) {
    errno = EINVAL;
    return -1;
  }
  interval->tv_sec = 0;
  interval->tv_nsec = 10000000;
  return 0;
}

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
  (void)pid;
  (void)cpusetsize;
  (void)mask;
  return 0;
}
