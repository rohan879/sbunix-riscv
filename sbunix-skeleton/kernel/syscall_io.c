/* RF: File I/O syscall handlers and file lock infrastructure extracted
 * from trap.c. Read, write, open, close, dup, lseek, fstat, getdents,
 * fcntl, file locking, etc. */

#include <syscall_handlers.h>
#include <trap_internal.h>
#include <pathres.h>
#include <pipe.h>
#include <proc.h>
#include <vfs.h>
#include <ext2.h>
#include <riscv.h>
#include <string.h>
#include <printk.h>
#include <vm.h>
#include <kmalloc.h>
#include <kalloc.h>
#include <tty.h>
#include <tar.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* File lock infrastructure                                            */
/* ------------------------------------------------------------------ */

/* Global lock table -- teaching OS uses a flat array of lock entries.
 * POSIX locks are keyed by (path, pid); OFD locks by (path, open_file *). */
struct file_lock lock_table[MAX_FILE_LOCKS];

enum {
  KERN_O_CREAT = 0100,
  KERN_O_EXCL = 0200,
  KERN_O_TRUNC = 01000,
  KERN_O_DIRECTORY = 0200000,
  KERN_O_NOFOLLOW = 0400000,
  KERN_O_CLOEXEC = 02000000,
  KERN_O_CLOFORK = 040000000,
  KERN_AT_REMOVEDIR = 0x200,
  KERN_AT_SYMLINK_FOLLOW = 0x400,
};

#define PROCFS_FD_NOT_LINK    ((long)-4096)

static int path_has_trailing_slash(const char *path) {
    int len = strlen(path);
    while (len > 0 && path[len - 1] == '/')
        len--;
    return len > 0 && path[len] == '/';
}

static int vfs_stat_mode_io(const char *path, uint32_t *mode_out) {
    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(path, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops || !ops->stat)
        return -ENOENT;

    uint8_t st[128];
    int ret = ops->stat(mnt->fs_data, relpath, st);
    if (ret < 0)
        return ret;
    memcpy(mode_out, st + 16, 4);
    return 0;
}

static int require_resolved_dir(const char *path) {
    uint32_t mode;
    int ret = vfs_stat_mode_io(path, &mode);
    if (ret < 0)
        return ret;
    return ((mode & 0xF000) == 0x4000) ? 0 : -ENOTDIR;
}

static void set_open_fd_flags(int fd, int flags);

static int open_file_mode(struct open_file *f, uint32_t *mode_out) {
    if (!f || f->is_pipe || !f->ops || !f->ops->fstat)
        return -ENOTDIR;
    uint8_t st[128];
    int ret = f->ops->fstat(f->fs_data, f->private, st);
    if (ret < 0)
        return ret;
    memcpy(mode_out, st + 16, 4);
    return 0;
}

static long procfd_reopen_open_file(struct open_file *src, int flags,
                                    int trailing_slash) {
    uint32_t mode;
    int ret = open_file_mode(src, &mode);
    if (ret < 0)
        return (long)ret;
    int is_dir = ((mode & 0xF000) == 0x4000);
    if ((trailing_slash || (flags & KERN_O_DIRECTORY)) && !is_dir)
        return (long)-ENOTDIR;
    int accmode = flags & 3;
    if (is_dir && (accmode == 1 || accmode == 2))
        return (long)-EISDIR;
    if (is_dir && (flags & KERN_O_TRUNC))
        return (long)-EISDIR;
    if (!src->ops->reopen)
        return (long)-ENXIO;

    struct open_file *nf = vfs_alloc_file();
    if (!nf)
        return (long)-EMFILE;
    int newfd = alloc_fd();
    if (newfd < 0) {
        vfs_free_file(nf);
        return (long)-EMFILE;
    }

    void *priv = NULL;
    ret = src->ops->reopen(src->fs_data, src->private, flags, &priv);
    if (ret < 0) {
        vfs_free_file(nf);
        return (long)ret;
    }

    nf->ops = src->ops;
    nf->fs_data = src->fs_data;
    nf->private = priv;
    nf->offset = 0;
    nf->flags = flags;
    nf->is_pipe = 0;
    int plen = strlen(src->path);
    if (plen > 255)
        plen = 255;
    memcpy(nf->path, src->path, plen);
    nf->path[plen] = '\0';
    proc_ofile(current_proc)[newfd] = nf;
    set_open_fd_flags(newfd, flags);
    return (long)newfd;
}

static void set_open_fd_flags(int fd, int flags) {
    current_proc->fd_flags[fd] = 0;
    if (flags & KERN_O_CLOEXEC)
        current_proc->fd_flags[fd] |= 1;
    if (flags & KERN_O_CLOFORK)
        current_proc->fd_flags[fd] |= 2;
}

static struct proc *procfs_open_find_pid(pid_t pid) {
    if (current_proc && current_proc->pid == pid &&
        current_proc->state != UNUSED)
        return current_proc;
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->pid == pid && p->state != UNUSED)
            return p;
    }
    return NULL;
}

static int parse_uint_component(const char **pp, int *out) {
    const char *p = *pp;
    int val = 0;
    if (*p < '0' || *p > '9')
        return -1;
    while (*p >= '0' && *p <= '9') {
        if (val > 1000000)
            return -1;
        val = val * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    *out = val;
    return 0;
}

static int parse_procfs_fd_path(const char *path, pid_t *pid_out, int *fd_out) {
    const char *p = path;
    int pid = 0;
    int fd = 0;

    if (strncmp(p, "/proc/", 6) != 0)
        return 0;
    p += 6;
    if (strncmp(p, "self", 4) == 0 && p[4] == '/') {
        if (!current_proc)
            return 0;
        pid = current_proc->pid;
        p += 5;
    } else {
        if (parse_uint_component(&p, &pid) < 0 || *p != '/')
            return 0;
        p++;
    }
    if (strncmp(p, "fd/", 3) != 0)
        return 0;
    p += 3;
    if (parse_uint_component(&p, &fd) < 0 || *p != '\0')
        return 0;

    *pid_out = (pid_t)pid;
    *fd_out = fd;
    return 1;
}

static long open_procfs_fd_link(const char *resolved, int flags,
                                int trailing_slash) {
    pid_t pid;
    int oldfd;
    int matched = parse_procfs_fd_path(resolved, &pid, &oldfd);
    if (!matched)
        return PROCFS_FD_NOT_LINK;

    struct proc *target = procfs_open_find_pid(pid);
    if (!target)
        return (long)-ENOENT;
    if (oldfd < 0 || oldfd >= NOFILE || !proc_ofile(target)[oldfd])
        return (long)-ENOENT;
    if (target != current_proc)
        return (long)-EACCES;
    if ((flags & KERN_O_NOFOLLOW) && !trailing_slash)
        return (long)-ELOOP;

    struct open_file *src = proc_ofile(current_proc)[oldfd];
    if (!vfs_file_valid(src))
        return (long)-ENOENT;

    if (src->is_pipe) {
        int pipeid = src->pipe_id;
        if (trailing_slash || (flags & KERN_O_DIRECTORY))
            return (long)-ENOTDIR;
        if ((flags & KERN_O_CREAT) && (flags & KERN_O_EXCL))
            return (long)-EEXIST;
        if (pipeid < 0 || pipeid >= NPIPES || !pipes[pipeid].used)
            return (long)-ENOENT;

        int accmode = flags & 3;
        int wants_read = (accmode == 0 || accmode == 2);
        int wants_write = (accmode == 1 || accmode == 2);
        struct open_file *nf = vfs_alloc_file();
        if (!nf)
            return (long)-EMFILE;
        int newfd = alloc_fd();
        if (newfd < 0) {
            vfs_free_file(nf);
            return (long)-EMFILE;
        }

        nf->is_pipe = 1;
        nf->pipe_read = wants_read;
        nf->pipe_write = wants_write;
        nf->pipe_id = pipeid;
        nf->ops = NULL;
        nf->flags = flags;
        if (wants_read)
            pipes[pipeid].read_open++;
        if (wants_write)
            pipes[pipeid].write_open++;
        proc_ofile(current_proc)[newfd] = nf;
        set_open_fd_flags(newfd, flags);
        return (long)newfd;
    }

    /* BF: Reopen the underlying object through a filesystem clone hook.
     * Falling back to src->path would lose identity after replacement and
     * reset stateful devices such as /dev/ptmx. */
    return procfd_reopen_open_file(src, flags, trailing_slash);
}

/* Two ranges [s1, s1+l1) and [s2, s2+l2) overlap.
 * l_len=0 means "to EOF" -- treat as infinite extent.
 * RF: Made static -- only used within this module */
static int ranges_overlap(int64_t s1, int64_t l1, int64_t s2, int64_t l2) {
  int64_t e1 = (l1 == 0) ? (int64_t)0x7FFFFFFFFFFFFFFFLL : s1 + l1;
  int64_t e2 = (l2 == 0) ? (int64_t)0x7FFFFFFFFFFFFFFFLL : s2 + l2;
  return s1 < e2 && s2 < e1;
}

/* Check whether an existing lock conflicts with a proposed lock.
 * Locks conflict if: same file path, overlapping byte ranges,
 * at least one is a write lock, and different owner. */
/* RF: Made static -- only used within this module */
static int lock_conflicts(struct file_lock *lk, const char *path,
    short type, int64_t start, int64_t len, pid_t pid,
    struct open_file *ofd, int is_ofd) {
  if (!lk->used) return 0;
  if (strncmp(lk->path, path, 256) != 0) return 0;
  if (!ranges_overlap(lk->l_start, lk->l_len, start, len)) return 0;
  /* Both read locks never conflict (F_RDLCK = 0) */
  if (lk->l_type == 0 && type == 0) return 0;
  /* Same owner = no conflict */
  if (is_ofd) {
    if (lk->ofd == ofd) return 0;
  } else {
    if (lk->l_pid == pid && lk->ofd == 0) return 0;
  }
  return 1;
}

/* Set or release a file lock.  F_UNLCK removes matching locks; F_RDLCK/F_WRLCK
 * checks for conflicts then adds/replaces a lock entry.
 * Returns 0 on success, -11 (EAGAIN) on conflict. */
int do_file_setlk(const char *path, short type, int64_t start, int64_t len,
                   pid_t pid, struct open_file *ofd, int is_ofd, int wait) {
  if (type == 2) { /* F_UNLCK -- remove matching locks owned by caller */
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
      struct file_lock *lk = &lock_table[i];
      if (!lk->used) continue;
      if (strncmp(lk->path, path, 256) != 0) continue;
      if (!ranges_overlap(lk->l_start, lk->l_len, start, len)) continue;
      /* Must be same owner */
      if (is_ofd) {
        if (lk->ofd != ofd) continue;
      } else {
        if (lk->l_pid != pid || lk->ofd != 0) continue;
      }
      lk->used = 0;
    }
    return 0;
  }
  /* F_RDLCK or F_WRLCK -- check for conflicts first */
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (lock_conflicts(&lock_table[i], path, type, start, len,
                       pid, ofd, is_ofd))
      return -11; /* EAGAIN (teaching OS: wait variant also returns EAGAIN) */
  }
  /* Replace existing lock from same owner on same range, or find free slot */
  int free_slot = -1;
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    struct file_lock *lk = &lock_table[i];
    if (!lk->used) {
      if (free_slot < 0) free_slot = i;
      continue;
    }
    /* Replace if same owner, same path, overlapping range */
    if (strncmp(lk->path, path, 256) != 0) continue;
    if (!ranges_overlap(lk->l_start, lk->l_len, start, len)) continue;
    int same_owner = 0;
    if (is_ofd && lk->ofd == ofd) same_owner = 1;
    if (!is_ofd && lk->l_pid == pid && lk->ofd == 0) same_owner = 1;
    if (same_owner) {
      lk->l_type = type;
      lk->l_start = start;
      lk->l_len = len;
      return 0;
    }
  }
  if (free_slot < 0)
    return -11; /* No free lock slots -- report EAGAIN */
  struct file_lock *lk = &lock_table[free_slot];
  lk->used = 1;
  lk->l_type = type;
  lk->l_start = start;
  lk->l_len = len;
  lk->l_pid = is_ofd ? -1 : pid;
  lk->ofd = is_ofd ? ofd : 0;
  {
    int plen = strlen(path);
    if (plen > 255) plen = 255;
    memcpy(lk->path, path, plen);
    lk->path[plen] = '\0';
  }
  return 0;
}

/* Query whether a hypothetical lock would conflict.  If a conflict is found,
 * fills out_type/out_start/out_len/out_pid.  Otherwise sets *out_type = 2
 * (F_UNLCK) meaning "no conflict". */
int do_file_getlk(const char *path, short type, int64_t start, int64_t len,
                   pid_t pid, struct open_file *ofd, int is_ofd,
                   short *out_type, int64_t *out_start, int64_t *out_len,
                   pid_t *out_pid) {
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (lock_conflicts(&lock_table[i], path, type, start, len,
                       pid, ofd, is_ofd)) {
      *out_type = lock_table[i].l_type;
      *out_start = lock_table[i].l_start;
      *out_len = lock_table[i].l_len;
      *out_pid = lock_table[i].l_pid;
      return 0;
    }
  }
  /* No conflict */
  *out_type = 2; /* F_UNLCK */
  *out_start = 0;
  *out_len = 0;
  *out_pid = 0;
  return 0;
}

/* Release all POSIX locks owned by pid -- called on process exit. */
void file_locks_release_pid(pid_t pid) {
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (lock_table[i].used && lock_table[i].ofd == 0 &&
        lock_table[i].l_pid == pid)
      lock_table[i].used = 0;
  }
}

/* Release all OFD locks owned by a specific open_file -- called when the
 * last reference to an open_file is closed. */
void file_locks_release_ofd(struct open_file *ofd) {
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (lock_table[i].used && lock_table[i].ofd == ofd)
      lock_table[i].used = 0;
  }
}

/* ------------------------------------------------------------------ */
/* Syscall handlers: file I/O                                          */
/* ------------------------------------------------------------------ */

static int pending_signal_interrupts_io(void) {
    uint64_t deliverable = signal_deliverable_mask(current_proc);
    struct kern_sigaction *ksa = proc_sigactions(current_proc);

    for (int s = 1; s < 65 && deliverable; s++) {
        if (!(deliverable & signal_bit(s)))
            continue;
        if (ksa[s].handler > 1)
            return 1;
        if (ksa[s].handler == 0) {
            /* BF: Blocked pipe writers must leave the sleep loop for signals
             * that will run a handler or terminate on syscall return. */
            if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                continue;
            if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                continue;
            if (s == SIGCONT)
                continue;
            return 1;
        }
    }

    return 0;
}

long sys_write_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t ubuf = tf->a1;
    size_t len = (size_t)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;

    if (!f) return (long)-9; /* -EBADF */

    /* Check file was opened for writing (skip for pipes — they use pipe_write flag) */
    if (!f->is_pipe) {
        int accmode = f->flags & 3; /* O_ACCMODE */
        if (accmode == 0) /* O_RDONLY */
            return (long)-9; /* -EBADF */
    }

    if (f->is_pipe && f->pipe_write) {
        int pid = f->pipe_id;
        /* Check for broken pipe before writing */
        if (pipes[pid].read_open <= 0) {
            /* BF: Capture sender metadata for SA_SIGINFO */
            sig_set_pending(current_proc, SIGPIPE,
                            (int32_t)current_proc->pid, 0, 0);
            return (long)-32; /* -EPIPE */
        }
        size_t written = 0;
        while (written < len) {
            /* Re-check after sleeping: reader may have closed */
            if (pipes[pid].read_open <= 0) {
                /* BF: Capture sender metadata for SA_SIGINFO */
                sig_set_pending(current_proc, SIGPIPE,
                                (int32_t)current_proc->pid, 0, 0);
                return (written > 0) ? (long)written : (long)-32;
            }
            int avail = KPIPE_BUF - pipe_avail(pid);
            if (avail <= 0) {
                /* BF: Nonblocking pipe writes must report EAGAIN instead of
                 * sleeping when no buffer space is available. */
                if (f->flags & 04000)
                    return (written > 0) ? (long)written : (long)-EAGAIN;
                current_proc->state = SLEEPING;
                swtch(&current_proc->context, &scheduler_context);
                if (pending_signal_interrupts_io())
                    return (written > 0) ? (long)written : (long)-EINTR;
                continue;
            }
            int n = len - written;
            if (n > avail) n = avail;
            char kchunk[512];
            while (n > 0) {
                int chunk = n > (int)sizeof(kchunk) ? (int)sizeof(kchunk) : n;
                if (copyin(current_proc->pagetable, kchunk, ubuf + written, chunk) < 0) {
                    return (written > 0) ? (long)written : (long)-EFAULT;
                }
                for (int i = 0; i < chunk; i++) {
                    pipes[pid].buf[pipes[pid].wpos % KPIPE_BUF] = kchunk[i];
                    pipes[pid].wpos++;
                }
                written += chunk;
                n -= chunk;
            }
            for (struct proc *p = proc; p < &proc[NPROC]; p++) {
                if (p->state == SLEEPING && p != current_proc &&
                    p->waiting_for_uart != 2)
                    p->state = RUNNABLE;
            }
        }
        return (long)written;
    } else if (f && f->ops && f->ops->write) {
        /* O_APPEND: seek to end of file before each write */
        if ((f->flags & 02000) && f->ops->fstat) {
            uint8_t st[128];
            if (f->ops->fstat(f->fs_data, f->private, st) == 0) {
                int64_t fsize;
                memcpy(&fsize, st + 48, 8);
                f->offset = fsize;
            }
        }
        char kbuf[512];
        int64_t total = 0;
        while ((size_t)total < len) {
            uint64_t chunk = len - total;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            if (copyin(current_proc->pagetable, kbuf, ubuf + total, chunk) < 0) {
                return (total > 0) ? (long)total : (long)-EFAULT;
            }
            int64_t ret = f->ops->write(f->fs_data, f->private, kbuf, chunk,
                                        &f->offset);
            if (ret < 0) { return (total > 0) ? (long)total : (long)ret; }
            total += ret;
            if ((uint64_t)ret < chunk) break;
        }
        return (long)total;
    } else {
        return (long)-9; /* EBADF */
    }
}

long sys_read_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t ubuf = tf->a1;
    size_t len = (size_t)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;

    if (!f) return (long)-9; /* -EBADF */

    /* Check file was opened for reading (skip for pipes — they use pipe_read flag) */
    if (!f->is_pipe) {
        int accmode = f->flags & 3; /* O_ACCMODE */
        if (accmode == 1) /* O_WRONLY */
            return (long)-9; /* -EBADF */
    }

    if (f->is_pipe && f->pipe_read) {
        int pid = f->pipe_id;
        size_t read_bytes = 0;
        /* Block until at least one byte is available (or EOF) */
        while (pipe_avail(pid) == 0) {
            if (pipes[pid].write_open == 0) goto pipe_eof;
            /* BF: Nonblocking pipe reads must return EAGAIN while writers
             * remain open and no bytes are ready. */
            if (f->flags & 04000)
                return (long)-EAGAIN;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
            /* Check for pending signals after waking up:
             * - User handler (handler > 1): return -EINTR
             * - SIG_DFL terminating signal: break out to let deliver_signal kill us
             * - SIG_IGN / SIG_DFL ignore-class: continue sleeping */
            {
                uint64_t deliverable = signal_deliverable_mask(current_proc);
                struct kern_sigaction *sa = proc_sigactions(current_proc);
                for (int s = 1; s < 65 && deliverable; s++) {
                    if (deliverable & signal_bit(s)) {
                        if (sa[s].handler > 1) /* user handler */
                            return (long)-4; /* -EINTR */
                        if (sa[s].handler == 0) { /* SIG_DFL */
                            /* Ignore-class: SIGCHLD, SIGURG, SIGWINCH (continue sleeping) */
                            if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                                continue;
                            /* Stop-class: SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU
                             * (will be handled by deliver_signal, but shouldn't terminate) */
                            if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                                continue;
                            /* SIGCONT: resume, not terminate */
                            if (s == SIGCONT)
                                continue;
                            /* All other SIG_DFL signals terminate; break out so
                             * deliver_signal can kill the process */
                            goto pipe_eof;
                        }
                        /* SIG_IGN (handler == 1): continue sleeping */
                    }
                }
            }
        }
        /* Drain all available bytes (up to len) without re-blocking */
        while (read_bytes < len && pipe_avail(pid) > 0) {
            char kc = pipes[pid].buf[pipes[pid].rpos % KPIPE_BUF];
            pipes[pid].rpos++;
            if (copyout(current_proc->pagetable, ubuf + read_bytes, &kc, 1) < 0) {
                return (read_bytes > 0) ? (long)read_bytes : (long)-EFAULT;
            }
            read_bytes++;
        }
        /* Wake any blocked pipe writers since we freed buffer space */
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->state == SLEEPING && p != current_proc &&
                p->waiting_for_uart != 2)
                p->state = RUNNABLE;
        }
pipe_eof:
        return (long)read_bytes;
    } else if (f && f->ops && f->ops->read) {
        char kbuf[512];
        int64_t total = 0;
        while ((size_t)total < len) {
            uint64_t chunk = len - total;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            int64_t ret = f->ops->read(f->fs_data, f->private, kbuf, chunk,
                                       &f->offset);
            if (ret < 0) { return (total > 0) ? (long)total : (long)ret; }
            if (ret == 0) break;
            if (copyout(current_proc->pagetable, ubuf + total, kbuf, ret) < 0) {
                return (total > 0) ? (long)total : (long)-EFAULT;
            }
            total += ret;
            if ((uint64_t)ret < chunk) break;
        }
        return (long)total;
    } else {
        return (long)-9; /* EBADF */
    }
}

long sys_openat_handler(struct trapframe *tf) {
    int dirfd = (int)tf->a0;
    int flags = (int)tf->a2;

    char kpath[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a1, kpath, sizeof(kpath), 0);
      if (pr < 0) return (long)pr; }
    int trailing_slash = path_has_trailing_slash(kpath);

    char resolved[KPATH_MAX];
    /* BF: Propagate dirfd resolution errors before VFS dispatch. */
    { int rr = resolve_path_at(dirfd, kpath, resolved);
      if (rr < 0) return (long)rr; }

    /* /proc/self/fd/N is a magic reopen point.  Regular files and stateful
     * devices get fresh open descriptions for the same object; pipes are
     * reopened as new endpoints against the same pipe object. */
    {
        long procfd = open_procfs_fd_link(resolved, flags, trailing_slash);
        if (procfd != PROCFS_FD_NOT_LINK)
            return procfd;
    }

    /* Resolve symlinks: intermediate always, final unless O_NOFOLLOW */
    {
        int nofollow = (flags & KERN_O_NOFOLLOW);
        int symret = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS,
                                              !nofollow || trailing_slash);
        if (symret == -ENOENT && (flags & KERN_O_CREAT) && !nofollow &&
            !trailing_slash) {
            /* BF: A missing final component under O_CREAT is not an error;
             * preserve intermediate symlink resolution and let the backend
             * create the new file. */
            { int rr = resolve_path_at(dirfd, kpath, resolved);
              if (rr < 0) return (long)rr; }
            symret = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS, 0);
        }
        /* BF: Propagate all negative errors, not just ELOOP */
        if (symret < 0)
            return (long)symret;
    }
    if (trailing_slash) {
        int ret = require_resolved_dir(resolved);
        if (ret < 0)
            return (long)ret;
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops) {
        return (long)-2; /* -ENOENT */
    }

    /* BF: Pre-open directory/flag validation.  These checks run BEFORE
     * ops->open() so that flags like O_TRUNC do not modify the file
     * when the open should have failed (e.g., O_DIRECTORY on a regular
     * file, or O_CREAT on an existing directory). */
    {
        int is_dir = vfs_is_dir(resolved);
        /* Reject O_DIRECTORY on a non-directory before any side-effects */
        if ((flags & KERN_O_DIRECTORY) && !is_dir)
            return (long)-20; /* -ENOTDIR */
        /* Reject O_WRONLY/O_RDWR on directories */
        if (is_dir) {
            int accmode = flags & 3;
            if (accmode == 1 || accmode == 2) /* O_WRONLY or O_RDWR */
                return (long)-21; /* -EISDIR */
        }
        /* Reject O_CREAT on an existing directory (POSIX: EISDIR) */
        if (is_dir && (flags & KERN_O_CREAT))
            return (long)-21; /* -EISDIR */
    }

    void *priv = NULL;
    int ret = ops->open(mnt->fs_data, relpath, flags, &priv);
    if (ret < 0) {
        return (long)ret;
    } else {
        struct open_file *f = vfs_alloc_file();
        if (!f) {
            if (ops->close) ops->close(mnt->fs_data, priv);
            return (long)-12;
        }
        int fd = alloc_fd();
        if (fd < 0) {
            if (ops->close) ops->close(mnt->fs_data, priv);
            vfs_free_file(f);
            return (long)-24; /* -EMFILE: too many open files */
        }
        f->ops = ops;
        f->fs_data = mnt->fs_data;
        f->private = priv;
        f->offset = 0;
        f->flags = flags;
        f->is_pipe = 0;
        {
          int plen = strlen(resolved);
          if (plen > 255) plen = 255;
          memcpy(f->path, resolved, plen);
          f->path[plen] = '\0';
        }
        proc_ofile(current_proc)[fd] = f;
        /* ENH: Propagate O_CLOEXEC and O_CLOFORK open flags to per-fd
         * flags so that F_GETFD/F_SETFD and fork/exec use fd_flags. */
        current_proc->fd_flags[fd] = 0;
        if (flags & KERN_O_CLOEXEC)
            current_proc->fd_flags[fd] |= 1; /* FD_CLOEXEC */
        if (flags & KERN_O_CLOFORK)
            current_proc->fd_flags[fd] |= 2; /* FD_CLOFORK */
        return (long)fd;
    }
}

long sys_close_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    if (fd >= 0 && fd < NOFILE && proc_ofile(current_proc)[fd]) {
        close_ofile(current_proc, fd);
        /* ENH: Clear per-fd flags when fd is closed */
        current_proc->fd_flags[fd] = 0;
        return (long)0;
    } else {
        return (long)-9; /* EBADF */
    }
}

long sys_dup3_handler(struct trapframe *tf) {
    int oldfd = (int)tf->a0;
    int newfd = (int)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (oldfd < 0 || oldfd >= NOFILE || !ftab[oldfd] ||
        newfd < 0 || newfd >= NOFILE) {
        return (long)-9; /* -EBADF */
    } else if (oldfd == newfd) {
        return (long)newfd;
    } else {
        if (ftab[newfd])
            close_ofile(current_proc, newfd);
        ftab[newfd] = ftab[oldfd];
        ftab[oldfd]->refcount++;
        if (ftab[oldfd]->is_pipe) {
            int pid = ftab[oldfd]->pipe_id;
            if (pid >= 0) {
                if (ftab[oldfd]->pipe_read)  pipes[pid].read_open++;
                if (ftab[oldfd]->pipe_write) pipes[pid].write_open++;
            }
        }
        /* ENH: Process O_CLOEXEC/O_CLOFORK flags in dup3.
         * POSIX: the new fd inherits only the flags passed in the
         * third argument, not the flags of the old fd. */
        {
            int flags = (int)tf->a2;
            current_proc->fd_flags[newfd] = 0;
            if (flags & KERN_O_CLOEXEC)
                current_proc->fd_flags[newfd] |= 1; /* FD_CLOEXEC */
            if (flags & KERN_O_CLOFORK)
                current_proc->fd_flags[newfd] |= 2; /* FD_CLOFORK */
        }
        return (long)newfd;
    }
}

long sys_getcwd_handler(struct trapframe *tf) {
    if (tf->a0 == 0) return (long)-EFAULT; /* -EFAULT: NULL buffer */
    size_t sz = (size_t)tf->a1;
    const char *cwd = current_proc->cwd;
    if (!cwd[0]) cwd = "/";
    int len = strlen(cwd);
    if ((size_t)(len + 1) > sz) {
        return (long)-34; /* -ERANGE: buffer too small */
    }
    if (copyout(current_proc->pagetable, tf->a0, (void *)cwd, len + 1) < 0) {
        return (long)-EFAULT; /* -EFAULT */
    }
    return (long)tf->a0; /* return buf pointer */
}

long sys_chdir_handler(struct trapframe *tf) {
    char kpath[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a0, kpath, sizeof(kpath), 0);
      if (pr < 0) return (long)pr; }
    char resolved[KPATH_MAX];
    { int rr = resolve_path(current_proc->cwd, kpath, resolved);
      if (rr < 0) return (long)rr; }
    /* BF: Propagate all negative errors from resolve_symlinks_in_path */
    { int sr = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS, 1);
      if (sr < 0) return (long)sr; }
    /* Check if path exists by trying stat */
    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops) return (long)-2; /* -ENOENT */
    uint8_t st[128];
    if (ops->stat(mnt->fs_data, relpath, st) != 0) return (long)-2; /* -ENOENT */
    /* Path exists — check if it's a directory */
    uint32_t mode;
    memcpy(&mode, st + 16, 4);
    if ((mode & 0xF000) != 0x4000) return (long)-20; /* -ENOTDIR */
    memcpy(current_proc->cwd, resolved, sizeof(current_proc->cwd));
    return (long)0;
}

/* DOC: fchdir uses the path snapshot from open() time.  If the directory
 * is renamed after the fd was opened, cwd will reflect the old name.
 * This is a known limitation — a correct fix would require tracking
 * inode-based cwd rather than path-based cwd. */
long sys_fchdir_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f) return (long)-9; /* EBADF */
    /* Use the stored path from the open file */
    if (f->path[0] == '\0') return (long)-22; /* EINVAL — no path */
    if (!vfs_is_dir(f->path)) return (long)-20; /* ENOTDIR */
    memcpy(current_proc->cwd, f->path, sizeof(current_proc->cwd));
    return (long)0;
}

long sys_lseek_handler(struct trapframe *tf) {
    int fd      = (int)tf->a0;
    long offset = (long)tf->a1;
    int whence  = (int)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f) {
        return (long)-9; /* EBADF */
    }
    if (f->is_pipe) {
        return (long)-29; /* ESPIPE */
    }
    int64_t cur = f->offset;
    int64_t new_off;
    if (whence == 0)       /* SEEK_SET */ new_off = (int64_t)offset;
    else if (whence == 1)  /* SEEK_CUR */ new_off = cur + (int64_t)offset;
    else if (whence == 2)  /* SEEK_END */ {
        int64_t file_size = 0;
        if (f->ops && f->ops->fstat) {
            uint8_t stbuf[128];
            if (f->ops->fstat(f->fs_data, f->private, stbuf) == 0)
                memcpy(&file_size, stbuf + 48, 8);
        }
        new_off = file_size + (int64_t)offset;
    }
    else { return (long)-22; /* EINVAL */ }
    if (new_off < 0) return (long)-22; /* EINVAL */
    f->offset = new_off;
    return (long)new_off;
}

/* BF: Share fd-backed stat construction between fstat() and
 * fstatat(fd, "", ..., AT_EMPTY_PATH). */
static long copy_fstat_to_user(int fd, uint64_t stat_uaddr) {
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f) {
        return (long)-9; /* EBADF */
    }
    if (!stat_uaddr) {
        return (long)-EFAULT; /* EFAULT */
    }
    if (f->is_pipe) {
        uint8_t kstat[128];
        memset(kstat, 0, 128);
        uint32_t mode = 0x1000 | 0600;
        uint32_t nlink = 1;
        uint64_t ino = (uint64_t)(f->pipe_id + 1);
        int32_t blksz = 4096;
        memcpy(kstat + 8, &ino, 8);
        memcpy(kstat + 16, &mode, 4);
        memcpy(kstat + 20, &nlink, 4);
        memcpy(kstat + 56, &blksz, 4);
        if (copyout(current_proc->pagetable, stat_uaddr, kstat, 128) < 0) {
            return (long)-EFAULT;
        }
        return (long)0;
    } else if (f->ops && f->ops->fstat) {
        uint8_t kstat[128];
        int ret = f->ops->fstat(f->fs_data, f->private, kstat);
        if (ret == 0) {
            if (copyout(current_proc->pagetable, stat_uaddr, kstat, 128) < 0) {
                return (long)-EFAULT;
            }
        }
        return (long)(int64_t)ret;
    } else {
        uint8_t kstat[128];
        memset(kstat, 0, 128);
        uint64_t dev = 1;
        uint64_t ino = (uint64_t)(fd + 1);
        uint32_t mode = 0x81A4;
        uint32_t nlink = 1;
        int32_t blksz = 4096;
        int64_t sec, nsec;
        get_wall_time(&sec, &nsec);
        memcpy(kstat + 0, &dev, 8);
        memcpy(kstat + 8, &ino, 8);
        memcpy(kstat + 16, &mode, 4);
        memcpy(kstat + 20, &nlink, 4);
        memcpy(kstat + 56, &blksz, 4);
        memcpy(kstat + 72, &sec, 8);
        memcpy(kstat + 80, &nsec, 8);
        memcpy(kstat + 88, &sec, 8);
        memcpy(kstat + 96, &nsec, 8);
        memcpy(kstat + 104, &sec, 8);
        memcpy(kstat + 112, &nsec, 8);
        if (copyout(current_proc->pagetable, stat_uaddr, kstat, 128) < 0) {
            return (long)-EFAULT;
        }
        return (long)0;
    }
}

long sys_fstat_handler(struct trapframe *tf) {
    return copy_fstat_to_user((int)tf->a0, tf->a1);
}

long sys_newfstatat_handler(struct trapframe *tf) {
    if (!tf->a1 || !tf->a2) {
        return (long)-1;
    }
    int at_flags = (int)tf->a3;
    if (at_flags & ~(0x100 | 0x1000))
        return (long)-EINVAL;
    char kpath[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a1, kpath, sizeof(kpath), 1);
      if (pr < 0) return (long)pr; }
    int trailing_slash = path_has_trailing_slash(kpath);
    /* BF: AT_EMPTY_PATH stats the dirfd argument directly when the supplied
     * path is a valid empty string. */
    if ((at_flags & 0x1000) && kpath[0] == '\0') {
        return copy_fstat_to_user((int)tf->a0, tf->a2);
    }
    char resolved[KPATH_MAX];
    /* BF: Propagate dirfd resolution errors before VFS dispatch. */
    { int rr = resolve_path_at((int)tf->a0, kpath, resolved);
      if (rr < 0) return (long)rr; }

    /* Resolve symlinks: intermediate always, final unless AT_SYMLINK_NOFOLLOW */
    {
        int nofollow = (at_flags & 0x100); /* AT_SYMLINK_NOFOLLOW = 0x100 */
        int symret = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS,
                                              !nofollow || trailing_slash);
        /* BF: Propagate all negative errors, not just ELOOP */
        if (symret < 0)
            return (long)symret;
    }
    if (trailing_slash) {
        int ret = require_resolved_dir(resolved);
        if (ret < 0)
            return (long)ret;
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ops && ops->stat) {
        uint8_t kstat[128];
        int ret = ops->stat(mnt->fs_data, relpath, kstat);
        if (ret == 0) {
            if (copyout(current_proc->pagetable, tf->a2, kstat, 128) < 0) {
                return (long)-EFAULT;
            }
        }
        return (long)(int64_t)ret;
    } else {
        return (long)-1;
    }
}

long sys_getdents64_handler(struct trapframe *tf) {
    int fd             = (int)tf->a0;
    uint64_t ubuf_va   = tf->a1;
    unsigned int count = (unsigned int)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;

    if (!f || f->is_pipe) {
        return (long)-9; /* -EBADF */
    }

    if (f->ops && f->ops->readdir) {
        if (count > 4096) count = 4096;
        char *kbuf = kalloc();
        if (!kbuf) {
            return (long)-12;
        }
        memset(kbuf, 0, 4096);

        int ret = f->ops->readdir(f->fs_data, f->private, kbuf, count,
                                  &f->offset);
        if (ret < 0) {
            kfree(kbuf);
            return (long)(int64_t)ret;
        } else {
            unsigned int written = (unsigned int)ret;
            if (ret == 0 && f->offset >= 0) {
                f->offset = -1024;
            }
            if (f->offset <= -1024) {
                const char *mnames[8];
                int nmounts = vfs_get_child_mounts(f->path, mnames, 8);
                int mstart = (int)(-(f->offset + 1024));
                for (int mi = mstart; mi < nmounts; mi++) {
                    int nl = strlen(mnames[mi]);
                    int rl = (19 + nl + 1 + 7) & ~7;
                    if (written + (unsigned int)rl > count) break;
                    char *d = kbuf + written;
                    memset(d, 0, rl);
                    uint64_t ino = (uint64_t)(5000 + mi);
                    int64_t doff = (int64_t)(-(1024 + mi + 1));
                    uint16_t rlen = (uint16_t)rl;
                    uint8_t dt = 4;
                    memcpy(d + 0, &ino, 8);
                    memcpy(d + 8, &doff, 8);
                    memcpy(d + 16, &rlen, 2);
                    memcpy(d + 18, &dt, 1);
                    memcpy(d + 19, mnames[mi], nl + 1);
                    written += (unsigned int)rl;
                    f->offset = (int64_t)(-(1024 + mi + 1));
                }
            }
            if (written > 0) {
                if (copyout(current_proc->pagetable, ubuf_va, kbuf, written) < 0) {
                    kfree(kbuf);
                    return (long)-EFAULT;
                }
            }
            kfree(kbuf);
            return (long)written;
        }
    } else {
        return (long)-1;
    }
}

long sys_writev_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t iov_uaddr = tf->a1;
    int iovcnt = (int)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f || iovcnt < 0) {
        return (long)-9;
    } else {
        if (iovcnt > 16) iovcnt = 16;
        struct { uint64_t base; uint64_t len; } kiov[16];
        if (copyin(current_proc->pagetable, kiov, iov_uaddr, iovcnt * 16) < 0) {
            return (long)-EFAULT;
        }
        int64_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            uint64_t buf_uaddr = kiov[i].base;
            uint64_t len = kiov[i].len;
            if (len == 0) continue;
            if (!buf_uaddr) {
                if (total == 0) total = -EFAULT;
                break;
            }
            if (f->is_pipe) {
                int pid = f->pipe_id;
                if (pid < 0 || !f->pipe_write) { total = -9; break; }
                if (pipes[pid].read_open <= 0) {
                    /* BF: Capture sender metadata for SA_SIGINFO */
                    sig_set_pending(current_proc, SIGPIPE,
                                    (int32_t)current_proc->pid, 0, 0);
                    if (total == 0) total = -32; /* -EPIPE */
                    goto writev_out;
                }
                char kchunk[512];
                uint64_t done = 0;
                while (done < len) {
                    uint64_t clen = len - done;
                    if (clen > sizeof(kchunk)) clen = sizeof(kchunk);
                    if (copyin(current_proc->pagetable, kchunk, buf_uaddr + done, clen) < 0) {
                        if (total == 0) total = -EFAULT;
                        goto writev_out;
                    }
                    for (uint64_t j = 0; j < clen; j++) {
                        while (pipe_avail(pid) >= KPIPE_BUF) {
                            if (!pipes[pid].read_open) break;
                            current_proc->state = SLEEPING;
                            swtch(&current_proc->context, &scheduler_context);
                            if (pending_signal_interrupts_io()) {
                                if (total == 0) total = -EINTR;
                                goto writev_out;
                            }
                        }
                        if (!pipes[pid].read_open) {
                            /* BF: Capture sender metadata for SA_SIGINFO */
                            sig_set_pending(current_proc, SIGPIPE,
                                            (int32_t)current_proc->pid, 0, 0);
                            if (total == 0) total = -32;
                            goto writev_out;
                        }
                        pipes[pid].buf[pipes[pid].wpos % KPIPE_BUF] = kchunk[j];
                        pipes[pid].wpos++;
                    }
                    done += clen;
                }
                total += (int64_t)len;
            } else if (f->ops && f->ops->write) {
                /* O_APPEND: seek to end before each iov write */
                if ((f->flags & 02000) && f->ops->fstat) {
                    uint8_t st[128];
                    if (f->ops->fstat(f->fs_data, f->private, st) == 0) {
                        int64_t fsize;
                        memcpy(&fsize, st + 48, 8);
                        f->offset = fsize;
                    }
                }
                char kchunk[512];
                uint64_t done = 0;
                while (done < len) {
                    uint64_t clen = len - done;
                    if (clen > sizeof(kchunk)) clen = sizeof(kchunk);
                    if (copyin(current_proc->pagetable, kchunk, buf_uaddr + done, clen) < 0) {
                        if (total == 0) total = -EFAULT;
                        goto writev_out;
                    }
                    int64_t n = f->ops->write(f->fs_data, f->private,
                                              kchunk, clen, &f->offset);
                    if (n < 0) { if (total == 0) total = n; goto writev_out; }
                    total += n;
                    done += n;
                    if ((uint64_t)n < clen) break;
                }
            } else {
                if (total == 0) total = -9;
                break;
            }
        }
writev_out:
        return (long)total;
    }
}

long sys_readv_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t iov_uaddr = tf->a1;
    int iovcnt = (int)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f || iovcnt < 0) {
        return (long)-9;
    } else {
        if (iovcnt > 16) iovcnt = 16;
        struct { uint64_t base; uint64_t len; } kiov[16];
        if (copyin(current_proc->pagetable, kiov, iov_uaddr, iovcnt * 16) < 0) {
            return (long)-EFAULT;
        }
        int64_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            uint64_t buf_uaddr = kiov[i].base;
            uint64_t len = kiov[i].len;
            if (len == 0) continue;
            if (!buf_uaddr) {
                if (total == 0) total = -EFAULT;
                break;
            }
            if (f->is_pipe) {
                int pid = f->pipe_id;
                if (pid < 0 || !f->pipe_read) { total = -9; break; }
                while (pipe_avail(pid) == 0 && pipes[pid].write_open)  {
                    current_proc->state = SLEEPING;
                    swtch(&current_proc->context, &scheduler_context);
                    /* Check for terminating signals after waking */
                    {
                        uint64_t deliverable = signal_deliverable_mask(current_proc);
                        struct kern_sigaction *ksa = proc_sigactions(current_proc);
                        for (int s = 1; s < 65 && deliverable; s++) {
                            if (deliverable & signal_bit(s)) {
                                if (ksa[s].handler > 1) { /* user handler */
                                    if (total == 0) total = -4;
                                    goto readv_out;
                                }
                                if (ksa[s].handler == 0) { /* SIG_DFL */
                                    if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                                        continue;
                                    if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                                        continue;
                                    if (s == SIGCONT)
                                        continue;
                                    /* Terminating: return to let deliver_signal kill */
                                    goto readv_out;
                                }
                            }
                        }
                    }
                }
                char kchunk[512];
                uint64_t j = 0;
                while (j < len && pipe_avail(pid) > 0) {
                    uint64_t clen = len - j;
                    if (clen > sizeof(kchunk)) clen = sizeof(kchunk);
                    uint64_t k;
                    for (k = 0; k < clen && pipe_avail(pid) > 0; k++) {
                        kchunk[k] = pipes[pid].buf[pipes[pid].rpos % KPIPE_BUF];
                        pipes[pid].rpos++;
                    }
                    if (k > 0 && copyout(current_proc->pagetable, buf_uaddr + j, kchunk, k) < 0) {
                        if (total == 0) total = -EFAULT;
                        goto readv_out;
                    }
                    j += k;
                }
                total += (int64_t)j;
            } else if (f->ops && f->ops->read) {
                char kchunk[512];
                uint64_t done = 0;
                while (done < len) {
                    uint64_t clen = len - done;
                    if (clen > sizeof(kchunk)) clen = sizeof(kchunk);
                    int64_t n = f->ops->read(f->fs_data, f->private,
                                             kchunk, clen, &f->offset);
                    if (n < 0) { if (total == 0) total = n; goto readv_out; }
                    if (n == 0) break;
                    if (copyout(current_proc->pagetable, buf_uaddr + done, kchunk, n) < 0) {
                        if (total == 0) total = -EFAULT;
                        goto readv_out;
                    }
                    total += n;
                    done += n;
                    if ((uint64_t)n < clen) break;
                }
            } else {
                if (total == 0) total = -9;
                break;
            }
        }
readv_out:
        return (long)total;
    }
}

/* pread64 — read at offset without changing file position */
long sys_pread64_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t ubuf = tf->a1;
    size_t count = (size_t)tf->a2;
    int64_t offset = (int64_t)tf->a3;
    struct open_file **ftab = proc_ofile(current_proc);
    if (fd < 0 || fd >= NOFILE || !ftab[fd]) return (long)-9;
    struct open_file *f = ftab[fd];
    if (f->is_pipe) return (long)-29; /* ESPIPE */
    if (!f->ops || !f->ops->read) return (long)-9;
    char kbuf[512];
    size_t total = 0;
    int64_t off = offset;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        int64_t nr = f->ops->read(f->fs_data, f->private, kbuf, chunk, &off);
        if (nr <= 0) break;
        if (copyout(current_proc->pagetable, ubuf + total, kbuf, (uint64_t)nr) < 0)
            return total > 0 ? (long)total : (long)-EFAULT;
        total += (size_t)nr;
        if ((size_t)nr < chunk) break;
    }
    return (long)total;
}

/* pwrite64 — write at offset without changing file position */
long sys_pwrite64_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t ubuf = tf->a1;
    size_t count = (size_t)tf->a2;
    int64_t offset = (int64_t)tf->a3;
    struct open_file **ftab = proc_ofile(current_proc);
    if (fd < 0 || fd >= NOFILE || !ftab[fd]) return (long)-9;
    struct open_file *f = ftab[fd];
    if (f->is_pipe) return (long)-29;
    if (!f->ops || !f->ops->write) return (long)-9;
    char kbuf[512];
    size_t total = 0;
    int64_t off = offset;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        if (copyin(current_proc->pagetable, kbuf, ubuf + total, chunk) < 0)
            return total > 0 ? (long)total : (long)-EFAULT;
        int64_t nw = f->ops->write(f->fs_data, f->private, kbuf, chunk, &off);
        if (nw <= 0) break;
        total += (size_t)nw;
        if ((size_t)nw < chunk) break;
    }
    return (long)total;
}

long sys_fcntl_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    int cmd = (int)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);

    if (fd < 0 || fd >= NOFILE || !ftab[fd])
        return (long)-9; /* EBADF */

    if (cmd == 0 || cmd == 1030 || cmd == 1031) {
        /* F_DUPFD (0), F_DUPFD_CLOEXEC (1030), or F_DUPFD_CLOFORK (1031)
         * — dup to lowest fd >= arg */
        int minfd = (int)tf->a2;
        if (minfd < 0 || minfd >= NOFILE)
            return (long)-22; /* EINVAL */
        for (int i = minfd; i < NOFILE; i++) {
            if (ftab[i] == 0) {
                ftab[i] = ftab[fd];
                ftab[fd]->refcount++;
                if (ftab[fd]->is_pipe) {
                    int pid = ftab[fd]->pipe_id;
                    if (pid >= 0) {
                        if (ftab[fd]->pipe_read)  pipes[pid].read_open++;
                        if (ftab[fd]->pipe_write) pipes[pid].write_open++;
                    }
                }
                /* ENH: Set per-fd flags based on F_DUPFD variant.
                 * F_DUPFD clears all flags; CLOEXEC/CLOFORK set their bit. */
                current_proc->fd_flags[i] = 0;
                if (cmd == 1030) /* F_DUPFD_CLOEXEC */
                    current_proc->fd_flags[i] |= 1; /* FD_CLOEXEC */
                if (cmd == 1031) /* F_DUPFD_CLOFORK */
                    current_proc->fd_flags[i] |= 2; /* FD_CLOFORK */
                return (long)i;
            }
        }
        return (long)-24; /* EMFILE */
    } else if (cmd == 1) { /* F_GETFD */
        /* ENH: Return per-fd flags (FD_CLOEXEC | FD_CLOFORK) from fd_flags
         * instead of deriving from open file flags.  POSIX requires F_GETFD
         * to return file descriptor flags, not file status flags. */
        return (long)current_proc->fd_flags[fd];
    } else if (cmd == 2) { /* F_SETFD */
        /* ENH: Store per-fd flags (FD_CLOEXEC=1, FD_CLOFORK=2) directly in
         * fd_flags.  Also sync O_CLOEXEC in the open file flags for backward
         * compat with the O_CLOEXEC-based close-on-exec path in execve. */
        current_proc->fd_flags[fd] = (uint8_t)((int)tf->a2 & 3);
        if ((int)tf->a2 & 1) /* FD_CLOEXEC */
            ftab[fd]->flags |= KERN_O_CLOEXEC;
        else
            ftab[fd]->flags &= ~KERN_O_CLOEXEC;
        return (long)0;
    } else if (cmd == 3) { /* F_GETFL */
        return (long)ftab[fd]->flags;
    } else if (cmd == 4) { /* F_SETFL */
        ftab[fd]->flags = (int)tf->a2;
        return (long)0;
    } else if (cmd == 5 || cmd == 36) {
        /* F_GETLK (5) / F_OFD_GETLK (36) -- query whether a lock would
         * conflict with existing locks.  Returns the conflicting lock info
         * in the user-supplied struct flock, or sets l_type=F_UNLCK if no
         * conflict exists. */
        struct { short l_type; short l_whence; int64_t l_start;
                 int64_t l_len; pid_t l_pid; } kfl;
        if (copyin(current_proc->pagetable, &kfl, tf->a2, sizeof(kfl)) < 0)
            return (long)-EFAULT; /* EFAULT */
        int is_ofd = (cmd == 36);
        /* Convert l_whence-relative offset to absolute (SEEK_CUR=1, SEEK_END=2) */
        if (kfl.l_whence == 1) { /* SEEK_CUR */
            kfl.l_start += ftab[fd]->offset;
        } else if (kfl.l_whence == 2) { /* SEEK_END */
            uint8_t stbuf[128];
            if (ftab[fd]->ops && ftab[fd]->ops->fstat &&
                ftab[fd]->ops->fstat(ftab[fd]->fs_data, ftab[fd]->private, stbuf) == 0) {
                int64_t file_size = *(int64_t *)(stbuf + 48);
                kfl.l_start += file_size;
            }
        }
        kfl.l_whence = 0;
        short ot; int64_t os, ol; pid_t op;
        do_file_getlk(ftab[fd]->path, kfl.l_type, kfl.l_start, kfl.l_len,
                       current_proc->pid, ftab[fd], is_ofd, &ot, &os, &ol, &op);
        kfl.l_type = ot; kfl.l_start = os; kfl.l_len = ol;
        kfl.l_pid = is_ofd ? -1 : op;
        kfl.l_whence = 0;
        if (copyout(current_proc->pagetable, tf->a2, &kfl, sizeof(kfl)) < 0)
            return (long)-EFAULT;
        return (long)0;
    } else if (cmd == 6 || cmd == 7 || cmd == 37 || cmd == 38) {
        /* F_SETLK (6) / F_SETLKW (7) / F_OFD_SETLK (37) / F_OFD_SETLKW (38)
         * -- set, upgrade, or release a file lock.  Wait variants also return
         * EAGAIN in this teaching OS (no blocking lock support). */
        struct { short l_type; short l_whence; int64_t l_start;
                 int64_t l_len; pid_t l_pid; } kfl;
        if (copyin(current_proc->pagetable, &kfl, tf->a2, sizeof(kfl)) < 0)
            return (long)-EFAULT;
        int is_ofd = (cmd == 37 || cmd == 38);
        int wait = (cmd == 7 || cmd == 38);
        /* Convert l_whence-relative offset to absolute */
        if (kfl.l_whence == 1) { /* SEEK_CUR */
            kfl.l_start += ftab[fd]->offset;
        } else if (kfl.l_whence == 2) { /* SEEK_END */
            uint8_t stbuf[128];
            if (ftab[fd]->ops && ftab[fd]->ops->fstat &&
                ftab[fd]->ops->fstat(ftab[fd]->fs_data, ftab[fd]->private, stbuf) == 0) {
                int64_t file_size = *(int64_t *)(stbuf + 48);
                kfl.l_start += file_size;
            }
        }
        return (long)do_file_setlk(ftab[fd]->path, kfl.l_type, kfl.l_start,
                                    kfl.l_len, current_proc->pid, ftab[fd],
                                    is_ofd, wait);
    } else if (cmd == 8) { /* F_SETOWN -- store owning pid (stub) */
        return (long)0;
    } else if (cmd == 9) { /* F_GETOWN -- return owning pid (stub) */
        return (long)current_proc->pid;
    } else {
        return (long)-22; /* EINVAL */
    }
}

long sys_faccessat_handler(struct trapframe *tf) {
    char kpath[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a1, kpath, sizeof(kpath), 0);
      if (pr < 0) return (long)pr; }
    int trailing_slash = path_has_trailing_slash(kpath);

    char resolved[KPATH_MAX];
    /* BF: Propagate dirfd resolution errors before VFS dispatch. */
    { int rr = resolve_path_at((int)tf->a0, kpath, resolved);
      if (rr < 0) return (long)rr; }
    /* BF: Propagate all negative errors from resolve_symlinks_in_path,
     * respecting AT_SYMLINK_NOFOLLOW in flags. */
    {
      int flags = (int)tf->a3;
      int nofollow = (flags & 0x100); /* AT_SYMLINK_NOFOLLOW = 0x100 */
      int sr = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS,
                                        !nofollow || trailing_slash);
      if (sr < 0) return (long)sr;
    }
    if (trailing_slash) {
        int ret = require_resolved_dir(resolved);
        if (ret < 0)
            return (long)ret;
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ops && ops->stat) {
        uint8_t st[128];
        int ret = ops->stat(mnt->fs_data, relpath, st);
        return (ret == 0) ? (long)0 : (long)-2;
    } else {
        return (long)-2;
    }
}

long sys_unlinkat_handler(struct trapframe *tf) {
    int dirfd = (int)tf->a0;
    int flags = (int)tf->a2;
    if (flags & ~KERN_AT_REMOVEDIR)
        return (long)-EINVAL;
    char kpath[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a1, kpath, sizeof(kpath), 0);
      if (pr < 0) return (long)pr; }
    int trailing_slash = path_has_trailing_slash(kpath);
    {
        char resolved[KPATH_MAX];
        /* BF: Propagate dirfd resolution errors before VFS dispatch. */
        { int rr = resolve_path_at(dirfd, kpath, resolved);
          if (rr < 0) return (long)rr; }
        /* BF: Propagate all negative errors from resolve_symlinks_in_path */
        { int sr = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS, 0);
          if (sr < 0) return (long)sr; }
        if (trailing_slash) {
            uint32_t mode_bits;
            int tr = vfs_stat_mode_io(resolved, &mode_bits);
            if (tr == 0 && ((mode_bits & 0xF000) != 0x4000))
                return (long)-ENOTDIR;
            if (tr < 0 && tr != -ENOENT)
                return (long)tr;
        }
        const char *relpath;
        struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
        struct vfs_ops *ops = mnt ? mnt->ops : NULL;
        if (flags & KERN_AT_REMOVEDIR) {
            if (ops && ops->rmdir) {
                int ret = ops->rmdir(mnt->fs_data, relpath);
                return (long)(int64_t)ret;
            } else {
                return (long)-30;
            }
        } else {
            if (ops && ops->unlink) {
                int ret = ops->unlink(mnt->fs_data, relpath);
                return (long)(int64_t)ret;
            } else {
                return (long)-30;
            }
        }
    }
}

long sys_linkat_handler(struct trapframe *tf) {
    /* a0 = olddirfd (AT_FDCWD), a1 = oldpath, a2 = newdirfd, a3 = newpath, a4 = flags */
    int olddirfd = (int)tf->a0;
    int newdirfd = (int)tf->a2;
    int flags = (int)tf->a4;
    if (flags & ~KERN_AT_SYMLINK_FOLLOW)
        return (long)-EINVAL;
    char kold[KPATH_MAX], knew[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a1, kold, sizeof(kold), 0);
      if (pr < 0) return (long)pr; }
    { int pr = copy_user_path_arg(tf->a3, knew, sizeof(knew), 0);
      if (pr < 0) return (long)pr; }
    int old_trailing = path_has_trailing_slash(kold);
    int new_trailing = path_has_trailing_slash(knew);

    char old_resolved[KPATH_MAX], new_resolved[KPATH_MAX];
    /* BF: Propagate dirfd resolution errors before VFS dispatch. */
    { int rr = resolve_path_at(olddirfd, kold, old_resolved);
      if (rr < 0) return (long)rr; }
    { int rr = resolve_path_at(newdirfd, knew, new_resolved);
      if (rr < 0) return (long)rr; }
    /* BF: linkat must follow symlinked parent directories while preserving
     * the final path component, otherwise /mnt/linkdir/file is treated as a
     * literal ext2 entry instead of resolving through the symlinked parent. */
    /* BF: AT_SYMLINK_FOLLOW switches only the old final component from
     * no-follow to follow; the new link name itself must remain literal. */
    { int sr = resolve_symlinks_in_path(old_resolved, KSYMLINK_MAX_HOPS,
                                        !!(flags & KERN_AT_SYMLINK_FOLLOW) ||
                                        old_trailing);
      if (sr < 0) return (long)sr; }
    { int sr = resolve_symlinks_in_path(new_resolved, KSYMLINK_MAX_HOPS, 0);
      if (sr < 0) return (long)sr; }
    if (old_trailing) {
        int tr = require_resolved_dir(old_resolved);
        if (tr < 0) return (long)tr;
    }
    if (new_trailing) {
        int tr = require_resolved_dir(new_resolved);
        if (tr < 0) return (long)tr;
    }

    /* Both paths must be on the same filesystem. */
    const char *old_rel, *new_rel;
    struct mount *old_mnt = vfs_lookup_mount_entry(old_resolved, &old_rel);
    struct mount *new_mnt = vfs_lookup_mount_entry(new_resolved, &new_rel);
    struct vfs_ops *old_ops = old_mnt ? old_mnt->ops : NULL;
    struct vfs_ops *new_ops = new_mnt ? new_mnt->ops : NULL;
    if (old_ops != new_ops ||
        (old_mnt && new_mnt && old_mnt->fs_data != new_mnt->fs_data))
        return (long)-18; /* EXDEV: cross-device link */
    if (old_ops && old_ops->stat) {
        uint8_t st[128];
        int ret = old_ops->stat(old_mnt->fs_data, old_rel, st);
        if (ret < 0)
            return (long)(int64_t)ret;
        uint32_t mode;
        memcpy(&mode, st + 16, 4);
        /* BF: Reject directory hardlinks before filesystem link operations. */
        if ((mode & 0xF000) == 0x4000)
            return (long)-EPERM;
    }
    if (ext2_is_ops(old_ops)) {
        /* BF: Hard-link the resolved ext2 inode directly.  Opening the
         * source rejects symlink inodes, but linkat without
         * AT_SYMLINK_FOLLOW must preserve the symlink itself. */
        uint32_t src_ino = 0;
        int lret = ext2_lookup_inode(old_mnt->fs_data, old_rel, &src_ino, NULL);
        if (lret < 0)
            return (long)lret;
        int ret = ext2_hardlink(new_mnt->fs_data, new_rel, src_ino);
        return (long)ret;
    } else if (old_ops && old_ops->link) {
        /* ENH: Generic VFS dispatch for non-ext2 backends (e.g. ramfs) */
        return (long)(int64_t)old_ops->link(old_mnt->fs_data, old_rel, new_rel);
    } else {
        return (long)-30; /* EROFS */
    }
}

long sys_renameat2_handler(struct trapframe *tf) {
    int olddirfd = (int)tf->a0;
    int newdirfd = (int)tf->a2;
    int flags = (int)tf->a4;
    if (flags != 0)
        return (long)-EINVAL;
    char kold[KPATH_MAX], knew[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a1, kold, sizeof(kold), 0);
      if (pr < 0) return (long)pr; }
    { int pr = copy_user_path_arg(tf->a3, knew, sizeof(knew), 0);
      if (pr < 0) return (long)pr; }
    int old_trailing = path_has_trailing_slash(kold);
    int new_trailing = path_has_trailing_slash(knew);

    char old_resolved[KPATH_MAX], new_resolved[KPATH_MAX];
    /* BF: Propagate dirfd resolution errors before VFS dispatch. */
    { int rr = resolve_path_at(olddirfd, kold, old_resolved);
      if (rr < 0) return (long)rr; }
    { int rr = resolve_path_at(newdirfd, knew, new_resolved);
      if (rr < 0) return (long)rr; }
    /* BF: renameat2 follows symlinked parent directories but must leave the
     * source and destination basenames unresolved so renaming a symlink still
     * acts on the link itself rather than the target. */
    { int sr = resolve_symlinks_in_path(old_resolved, KSYMLINK_MAX_HOPS, 0);
      if (sr < 0) return (long)sr; }
    { int sr = resolve_symlinks_in_path(new_resolved, KSYMLINK_MAX_HOPS, 0);
      if (sr < 0) return (long)sr; }
    if (old_trailing) {
        int tr = require_resolved_dir(old_resolved);
        if (tr < 0) return (long)tr;
    }
    if (new_trailing) {
        int tr = require_resolved_dir(new_resolved);
        if (tr < 0) return (long)tr;
    }

    const char *old_relpath, *new_relpath;
    struct mount *old_mnt = vfs_lookup_mount_entry(old_resolved, &old_relpath);
    struct mount *new_mnt = vfs_lookup_mount_entry(new_resolved, &new_relpath);
    struct vfs_ops *old_ops = old_mnt ? old_mnt->ops : NULL;
    struct vfs_ops *new_ops = new_mnt ? new_mnt->ops : NULL;

    if (old_ops != new_ops ||
        (old_mnt && new_mnt && old_mnt->fs_data != new_mnt->fs_data))
        return (long)-18; /* EXDEV */
    if (!old_ops || !old_ops->rename)
        return (long)-30; /* EROFS */

    return (long)(int64_t)old_ops->rename(old_mnt->fs_data, old_relpath,
                                          new_relpath);
}

long sys_mkdirat_handler(struct trapframe *tf) {
    int dirfd = (int)tf->a0;
    int mode = (int)tf->a2;
    char kpath[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a1, kpath, sizeof(kpath), 0);
      if (pr < 0) return (long)pr; }
    int trailing_slash = path_has_trailing_slash(kpath);
    {
        char resolved[KPATH_MAX];
        /* BF: Propagate dirfd resolution errors before VFS dispatch. */
        { int rr = resolve_path_at(dirfd, kpath, resolved);
          if (rr < 0) return (long)rr; }
        /* BF: Propagate all negative errors from resolve_symlinks_in_path */
        { int sr = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS, 0);
          if (sr < 0) return (long)sr; }
        if (trailing_slash) {
            uint32_t mode_bits;
            int tr = vfs_stat_mode_io(resolved, &mode_bits);
            if (tr == 0 && ((mode_bits & 0xF000) != 0x4000))
                return (long)-ENOTDIR;
            /* BF: mkdir("missing/") creates the missing final directory;
             * a trailing slash requires the created object to be a directory,
             * not that it already exists before mkdirat runs. */
            if (tr < 0 && tr != -ENOENT)
                return (long)tr;
        }
        const char *relpath;
        struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
        struct vfs_ops *ops = mnt ? mnt->ops : NULL;
        if (ops && ops->mkdir) {
            int ret = ops->mkdir(mnt->fs_data, relpath, mode);
            return (long)(int64_t)ret;
        } else {
            return (long)-30;
        }
    }
}

long sys_mknodat_handler(struct trapframe *tf) {
    int dirfd = (int)tf->a0;
    uint32_t mode = (uint32_t)tf->a2;
    uint32_t rdev = (uint32_t)tf->a3;
    char kpath[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a1, kpath, sizeof(kpath), 0);
      if (pr < 0) return (long)pr; }
    int trailing_slash = path_has_trailing_slash(kpath);
    {
        char resolved[KPATH_MAX];
        /* BF: Propagate dirfd resolution errors before VFS dispatch. */
        { int rr = resolve_path_at(dirfd, kpath, resolved);
          if (rr < 0) return (long)rr; }

        /* BF: mknodat must resolve intermediate symlinked directories. */
        { int sr = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS, 0);
          if (sr < 0) return (long)sr; }
        if (trailing_slash) {
            int tr = require_resolved_dir(resolved);
            if (tr < 0) return (long)tr;
        }

        const char *relpath;
        struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
        struct vfs_ops *ops = mnt ? mnt->ops : NULL;
        if (ext2_is_ops(ops)) {
            int ret = ext2_mknod(mnt->fs_data, relpath, mode, rdev);
            return (long)(int64_t)ret;
        } else if (ops && ops->open) {
            /* ENH: For in-memory filesystems (ramfs), create the file using
             * O_CREAT|O_EXCL.  S_ISFIFO mode bits are preserved as a stub
             * (stat reports FIFO type, but no pipe semantics). */
            void *priv = NULL;
            int ret = ops->open(mnt->fs_data, relpath, 0x40 | 0x80, &priv);
            if (ret >= 0 && ops->close) ops->close(mnt->fs_data, priv);
            return (long)(ret >= 0 ? 0 : ret);
        } else {
            return (long)-30;
        }
    }
}

long sys_readlinkat_handler(struct trapframe *tf) {
    char kpath[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a1, kpath, sizeof(kpath), 0);
      if (pr < 0) return (long)pr; }
    int trailing_slash = path_has_trailing_slash(kpath);

    int bufsiz = (int)tf->a3;
    if (bufsiz <= 0)
        return (long)-22;

    char resolved[KPATH_MAX];
    /* BF: Propagate dirfd resolution errors before VFS dispatch. */
    { int rr = resolve_path_at((int)tf->a0, kpath, resolved);
      if (rr < 0) return (long)rr; }
    /* BF: readlinkat must resolve intermediate symlinks but leave the final
     * path component untouched.  Without this, readlink("dirlink/child")
     * fails when dirlink is itself a symlink to a directory. */
    { int sr = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS,
                                        trailing_slash);
      if (sr < 0) return (long)sr; }
    if (trailing_slash) {
        int tr = require_resolved_dir(resolved);
        if (tr < 0) return (long)tr;
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops || !ops->readlink)
        return (long)-22; /* EINVAL */

    int maxread = bufsiz > EXT2_BLOCK_SIZE ? EXT2_BLOCK_SIZE : bufsiz;
    char *kbuf = (char *)kmalloc((uint64_t)maxread);
    if (!kbuf)
        return (long)-ENOMEM;

    int ret = ops->readlink(mnt->fs_data, relpath, kbuf, maxread);
    if (ret < 0)
        goto out;

    if (copyout(current_proc->pagetable, tf->a2, kbuf, (uint64_t)ret) < 0)
        ret = -EFAULT;

out:
    kmfree(kbuf, (uint64_t)maxread);
    return (long)ret;
}

long sys_symlinkat_handler(struct trapframe *tf) {
    char *ktarget;
    char klinkpath[KPATH_MAX];

    ktarget = (char *)kmalloc(KSYMLINK_MAX + 1);
    if (!ktarget)
        return (long)-ENOMEM;

    /* BF: Match the advertised SYMLINK_MAX capacity and distinguish a
     * too-long target from an invalid user pointer.  The target is stored
     * opaquely, but empty target strings still fail with ENOENT. */
    { int pr = copy_user_path_arg(tf->a0, ktarget, KSYMLINK_MAX + 1, 0);
      if (pr < 0) { kmfree(ktarget, KSYMLINK_MAX + 1); return (long)pr; } }
    { int pr = copy_user_path_arg(tf->a2, klinkpath, sizeof(klinkpath), 0);
      if (pr < 0) { kmfree(ktarget, KSYMLINK_MAX + 1); return (long)pr; } }
    int trailing_slash = path_has_trailing_slash(klinkpath);

    char resolved[KPATH_MAX];
    /* BF: Propagate dirfd resolution errors before VFS dispatch. */
    { int rr = resolve_path_at((int)tf->a1, klinkpath, resolved);
      if (rr < 0) {
          kmfree(ktarget, KSYMLINK_MAX + 1);
          return (long)rr;
      } }

    /* BF: symlinkat must resolve intermediate symlinked directories while
     * preserving the final component (the new link path itself).  Without
     * this, creating a link under a symlinked parent directory stores the
     * entry at the literal path instead of the resolved target directory. */
    { int sr = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS, 0);
      if (sr < 0) {
          kmfree(ktarget, KSYMLINK_MAX + 1);
          return (long)sr;
      } }
    if (trailing_slash) {
        int tr = require_resolved_dir(resolved);
        if (tr < 0) {
            kmfree(ktarget, KSYMLINK_MAX + 1);
            return (long)tr;
        }
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    int ret = -30; /* EROFS */
    if (ops && ops->symlink)
        ret = ops->symlink(mnt->fs_data, ktarget, relpath);
    kmfree(ktarget, KSYMLINK_MAX + 1);
    return (long)(int64_t)ret;
}

/* BF: Mirror of ramfs inode and private structs from ramfs.c.
 * Needed by fchmod/fchmodat handlers to access ramfs inode fields
 * (mode) directly from syscall_io.c.  Layout must exactly match
 * the real struct in ramfs.c (field order, sizes, padding).
 * RF: File-local copies (same struct name, no linker conflict). */
struct ramfs_inode_trap {
  uint64_t ino;             /* offset 0 */
  int      type;            /* offset 8 (enum ramfs_type = int) */
  uint32_t mode;            /* offset 12 */
  int      nlink;           /* offset 16 */
  uint64_t size;            /* offset 24 (after 4-byte padding) */
  uint64_t atime_sec;       /* offset 32 */
  uint64_t mtime_sec;       /* offset 40 */
  uint64_t ctime_sec;       /* offset 48 */
  /* remaining fields (blocks[], nblocks, children[], etc.) omitted */
};
struct ramfs_private {
  struct ramfs_inode_trap *inode;
};

long sys_fchmodat_handler(struct trapframe *tf) {
    /* a0 = dirfd (AT_FDCWD), a1 = user path, a2 = mode, a3 = flags */
    char kpath[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a1, kpath, sizeof(kpath), 0);
      if (pr < 0) return (long)pr; }
    int trailing_slash = path_has_trailing_slash(kpath);

    /* Resolve path and verify it exists via stat */
    char resolved[KPATH_MAX];
    /* BF: Propagate dirfd resolution errors before VFS dispatch. */
    { int rr = resolve_path_at((int)tf->a0, kpath, resolved);
      if (rr < 0) return (long)rr; }

    /* BF: Propagate all negative errors from resolve_symlinks_in_path,
     * respecting AT_SYMLINK_NOFOLLOW in flags. */
    {
      int flags = (int)tf->a3;
      int nofollow = (flags & 0x100); /* AT_SYMLINK_NOFOLLOW = 0x100 */
      int sr = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS,
                                        !nofollow || trailing_slash);
      if (sr < 0) return (long)sr;
    }
    if (trailing_slash) {
        int tr = require_resolved_dir(resolved);
        if (tr < 0) return (long)tr;
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ops && ops->stat) {
        uint8_t st[128];
        int ret = ops->stat(mnt->fs_data, relpath, st);
        if (ret < 0)
            return (long)-2; /* ENOENT */
    } else {
        return (long)-2; /* ENOENT */
    }
    /* ext2: actually set the mode on disk */
    if (ext2_is_ops(ops) && ops->open) {
        void *priv = NULL;
        if (ops->open(mnt->fs_data, relpath, 0, &priv) >= 0) {
            struct ext2_open_private *ep = (struct ext2_open_private *)priv;
            ext2_fchmod_ino(mnt->fs_data, ep->ino, (uint32_t)tf->a2);
            if (ops->close) ops->close(mnt->fs_data, priv);
        }
    }
    /* BF: For ramfs files, update inode mode in memory.  Without this,
     * fchmodat on /tmp paths was a no-op. */
    if (ops == &ramfs_ops && ops->open) {
        void *priv = NULL;
        if (ops->open(mnt->fs_data, relpath, 0, &priv) >= 0) {
            struct ramfs_private *rp = (struct ramfs_private *)priv;
            rp->inode->mode = (uint32_t)tf->a2 & 07777;
            if (ops->close) ops->close(mnt->fs_data, priv);
        }
    }
    return 0;
}

long sys_fchmod_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint32_t mode = (uint32_t)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (fd < 0 || fd >= NOFILE || !ftab[fd])
        return (long)-9; /* EBADF */
    struct open_file *f = ftab[fd];
    if (f->is_pipe) return (long)-22; /* EINVAL */
    /* ext2 files: update inode mode on disk */
    if (ext2_is_ops(f->ops) && f->private) {
        struct ext2_open_private *priv = (struct ext2_open_private *)f->private;
        return (long)ext2_fchmod_ino(f->fs_data, priv->ino, mode);
    }
    /* BF: For ramfs files, update the in-memory inode mode.  Without
     * this, fchmod on /tmp files was a silent no-op, causing tests
     * that verify permission changes to fail. */
    if (f->ops == &ramfs_ops && f->private) {
        struct ramfs_private *rp = (struct ramfs_private *)f->private;
        rp->inode->mode = mode & 07777; /* keep only permission bits */
        return 0;
    }
    return 0; /* other filesystems: no-op */
}

long sys_ftruncate_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t length = (uint64_t)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (fd < 0 || fd >= NOFILE || !ftab[fd])
        return (long)-9; /* EBADF */
    struct open_file *f = ftab[fd];
    if (f->is_pipe) return (long)-22; /* EINVAL */
    int accmode = f->flags & 3;
    if (accmode == 0) return (long)-9; /* EBADF: read-only fd */
    if (f->ops && f->ops->ftruncate)
        return (long)f->ops->ftruncate(f->fs_data, f->private, length);
    return (long)-30; /* EROFS: read-only filesystems */
}
