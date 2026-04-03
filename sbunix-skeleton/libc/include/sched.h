#ifndef _SCHED_H
#define _SCHED_H

#include <sys/types.h>
#include <time.h>
#include <string.h>

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_SPORADIC 3

#ifndef _POSIX_SPORADIC_SERVER
#define _POSIX_SPORADIC_SERVER 200112L
#endif

struct sched_param {
  int sched_priority;
  int sched_ss_low_priority;
  struct timespec sched_ss_repl_period;
  struct timespec sched_ss_init_budget;
  int sched_ss_max_repl;
};

#define CPU_SETSIZE 128
typedef struct {
  unsigned long __bits[(CPU_SETSIZE + (8 * sizeof(unsigned long)) - 1) /
                       (8 * sizeof(unsigned long))];
} cpu_set_t;

#define CPU_ZERO(setp) memset((setp), 0, sizeof(cpu_set_t))
#define CPU_SET(cpu, setp) \
  ((setp)->__bits[(cpu) / (8 * sizeof(unsigned long))] |= \
   (1UL << ((cpu) % (8 * sizeof(unsigned long)))))

int sched_yield(void);
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_setparam(pid_t pid, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_rr_get_interval(pid_t pid, struct timespec *interval);
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);

#endif /* _SCHED_H */
