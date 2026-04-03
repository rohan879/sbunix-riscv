#ifndef _SPAWN_H
#define _SPAWN_H

#include <sched.h>
#include <signal.h>
#include <sys/types.h>

typedef struct {
  short sa_flags;
  pid_t sa_pgroup;
  sigset_t sa_sigdefault;
  sigset_t sa_sigmask;
  int sa_schedpolicy;
  struct sched_param sa_schedparam;
} posix_spawnattr_t;

typedef struct {
  int used;
  int action_count;
  int action_cap;
  void *actions;
} posix_spawn_file_actions_t;

#define POSIX_SPAWN_RESETIDS      0x01
#define POSIX_SPAWN_SETPGROUP     0x02
#define POSIX_SPAWN_SETSIGDEF     0x04
#define POSIX_SPAWN_SETSIGMASK    0x08
#define POSIX_SPAWN_SETSCHEDPARAM 0x10
#define POSIX_SPAWN_SETSCHEDULER  0x20
#define POSIX_SPAWN_SETSID        0x40

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]);
int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]);

int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup);
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup);
int posix_spawnattr_getschedparam(const posix_spawnattr_t *attr,
                                  struct sched_param *param);
int posix_spawnattr_setschedparam(posix_spawnattr_t *attr,
                                  const struct sched_param *param);
int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *attr, int *policy);
int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int policy);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *set);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *set);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *set);
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *set);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions,
                                     int fd, const char *path,
                                     int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions,
                                      int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions,
                                     int fd, int newfd);
int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *actions,
                                      const char *path);
int posix_spawn_file_actions_addfchdir(posix_spawn_file_actions_t *actions,
                                       int fd);

#endif
