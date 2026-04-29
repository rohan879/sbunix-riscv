#include <spawn.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

enum {
  FA_OPEN = 1,
  FA_CLOSE = 2,
  FA_DUP2 = 3,
  FA_CHDIR = 4,
  FA_FCHDIR = 5,
};

struct file_action {
  int type;
  int fd;
  int newfd;
  int oflag;
  mode_t mode;
  char *path;
};

static int ensure_actions_cap(posix_spawn_file_actions_t *actions) {
  if (actions->action_count < actions->action_cap)
    return 0;
  int newcap = actions->action_cap ? actions->action_cap * 2 : 8;
  struct file_action *tmp = (struct file_action *)realloc(actions->actions,
                                  (size_t)newcap * sizeof(struct file_action));
  if (!tmp)
    return -1;
  actions->actions = tmp;
  actions->action_cap = newcap;
  return 0;
}

int posix_spawnattr_init(posix_spawnattr_t *attr) {
  if (!attr)
    return EINVAL;
  memset(attr, 0, sizeof(*attr));
  attr->sa_schedpolicy = SCHED_OTHER;
  return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr) {
  if (!attr)
    return EINVAL;
  memset(attr, 0, sizeof(*attr));
  return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags) {
  if (!attr || !flags)
    return EINVAL;
  *flags = attr->sa_flags;
  return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags) {
  if (!attr)
    return EINVAL;
  attr->sa_flags = flags;
  return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup) {
  if (!attr || !pgroup)
    return EINVAL;
  *pgroup = attr->sa_pgroup;
  return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup) {
  if (!attr)
    return EINVAL;
  attr->sa_pgroup = pgroup;
  return 0;
}

int posix_spawnattr_getschedparam(const posix_spawnattr_t *attr,
                                  struct sched_param *param) {
  if (!attr || !param)
    return EINVAL;
  *param = attr->sa_schedparam;
  return 0;
}

int posix_spawnattr_setschedparam(posix_spawnattr_t *attr,
                                  const struct sched_param *param) {
  if (!attr || !param)
    return EINVAL;
  attr->sa_schedparam = *param;
  return 0;
}

int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *attr, int *policy) {
  if (!attr || !policy)
    return EINVAL;
  *policy = attr->sa_schedpolicy;
  return 0;
}

int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int policy) {
  if (!attr)
    return EINVAL;
  attr->sa_schedpolicy = policy;
  return 0;
}

int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *set) {
  if (!attr || !set)
    return EINVAL;
  *set = attr->sa_sigdefault;
  return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *set) {
  if (!attr || !set)
    return EINVAL;
  attr->sa_sigdefault = *set;
  return 0;
}

int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *set) {
  if (!attr || !set)
    return EINVAL;
  *set = attr->sa_sigmask;
  return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *set) {
  if (!attr || !set)
    return EINVAL;
  attr->sa_sigmask = *set;
  return 0;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions) {
  if (!actions)
    return EINVAL;
  memset(actions, 0, sizeof(*actions));
  actions->used = 1;
  return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions) {
  if (!actions)
    return EINVAL;
  struct file_action *fa = (struct file_action *)actions->actions;
  for (int i = 0; i < actions->action_count; i++) {
    if (fa[i].path)
      free(fa[i].path);
  }
  free(actions->actions);
  memset(actions, 0, sizeof(*actions));
  return 0;
}

static int add_action_common(posix_spawn_file_actions_t *actions, int type,
                             int fd, int newfd, int oflag, mode_t mode,
                             const char *path) {
  if (!actions || !actions->used)
    return EINVAL;
  if (ensure_actions_cap(actions) < 0)
    return ENOMEM;
  struct file_action *fa = (struct file_action *)actions->actions;
  struct file_action *a = &fa[actions->action_count++];
  memset(a, 0, sizeof(*a));
  a->type = type;
  a->fd = fd;
  a->newfd = newfd;
  a->oflag = oflag;
  a->mode = mode;
  if (path) {
    a->path = strdup(path);
    if (!a->path)
      return ENOMEM;
  }
  return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions,
                                     int fd, const char *path,
                                     int oflag, mode_t mode) {
  return add_action_common(actions, FA_OPEN, fd, -1, oflag, mode, path);
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions,
                                      int fd) {
  return add_action_common(actions, FA_CLOSE, fd, -1, 0, 0, (void *)0);
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions,
                                     int fd, int newfd) {
  return add_action_common(actions, FA_DUP2, fd, newfd, 0, 0, (void *)0);
}

int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *actions,
                                      const char *path) {
  return add_action_common(actions, FA_CHDIR, -1, -1, 0, 0, path);
}

int posix_spawn_file_actions_addfchdir(posix_spawn_file_actions_t *actions,
                                       int fd) {
  return add_action_common(actions, FA_FCHDIR, fd, -1, 0, 0, (void *)0);
}

static void apply_actions(const posix_spawn_file_actions_t *actions) {
  if (!actions || !actions->actions)
    return;
  struct file_action *fa = (struct file_action *)actions->actions;
  for (int i = 0; i < actions->action_count; i++) {
    switch (fa[i].type) {
      case FA_OPEN: {
        int tmp = open(fa[i].path, fa[i].oflag, fa[i].mode);
        if (tmp >= 0) {
          if (tmp != fa[i].fd) {
            dup2(tmp, fa[i].fd);
            close(tmp);
          }
        }
        break;
      }
      case FA_CLOSE:
        close(fa[i].fd);
        break;
      case FA_DUP2:
        dup2(fa[i].fd, fa[i].newfd);
        break;
      case FA_CHDIR:
        chdir(fa[i].path);
        break;
      case FA_FCHDIR:
        fchdir(fa[i].fd);
        break;
    }
  }
}

static void apply_spawn_attrs(const posix_spawnattr_t *attrp) {
  if (!attrp)
    return;

  if (attrp->sa_flags & POSIX_SPAWN_SETSIGMASK)
    sigprocmask(SIG_SETMASK, &attrp->sa_sigmask, (sigset_t *)0);

  if (attrp->sa_flags & POSIX_SPAWN_SETSIGDEF) {
    for (int sig = 1; sig < NSIG; sig++) {
      if (sig == SIGKILL || sig == SIGSTOP)
        continue;
      if (sigismember(&attrp->sa_sigdefault, sig) == 1)
        signal(sig, SIG_DFL);
    }
  }

  if (attrp->sa_flags & POSIX_SPAWN_SETSID)
    setsid();

  if (attrp->sa_flags & POSIX_SPAWN_SETPGROUP) {
    pid_t pgroup = attrp->sa_pgroup;
    if (pgroup == 0)
      pgroup = getpid();
    setpgid(0, pgroup);
  }
}

static int spawn_impl(pid_t *pid, const char *path, const char *file,
                      const posix_spawn_file_actions_t *file_actions,
                      const posix_spawnattr_t *attrp,
                      char *const argv[], char *const envp[], int use_path) {
  pid_t cpid = fork();
  if (cpid < 0)
    return errno;
  if (cpid == 0) {
    apply_spawn_attrs(attrp);
    apply_actions(file_actions);
    if (use_path)
      execvp(file ? file : path, argv);
    else
      execve(path, argv, envp ? envp : environ);
    _exit(127);
  }
  if (pid)
    *pid = cpid;
  return 0;
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]) {
  if (!path || !argv)
    return EINVAL;
  return spawn_impl(pid, path, (const char *)0, file_actions, attrp, argv, envp, 0);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]) {
  (void)envp;
  if (!file || !argv)
    return EINVAL;
  return spawn_impl(pid, (const char *)0, file, file_actions, attrp, argv, environ, 1);
}
