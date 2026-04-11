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

/* ------------------------------------------------------------------ */
/* File lock infrastructure                                            */
/* ------------------------------------------------------------------ */

/* Global lock table -- teaching OS uses a flat array of lock entries.
 * POSIX locks are keyed by (path, pid); OFD locks by (path, open_file *). */
struct file_lock lock_table[MAX_FILE_LOCKS];

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
            current_proc->sigpending |= signal_bit(SIGPIPE);
            return (long)-32; /* -EPIPE */
        }
        size_t written = 0;
        while (written < len) {
            /* Re-check after sleeping: reader may have closed */
            if (pipes[pid].read_open <= 0) {
                current_proc->sigpending |= signal_bit(SIGPIPE);
                return (written > 0) ? (long)written : (long)-32;
            }
            int avail = KPIPE_BUF - pipe_avail(pid);
            if (avail <= 0) {
                current_proc->state = SLEEPING;
                swtch(&current_proc->context, &scheduler_context);
                continue;
            }
            int n = len - written;
            if (n > avail) n = avail;
            char kchunk[512];
            while (n > 0) {
                int chunk = n > (int)sizeof(kchunk) ? (int)sizeof(kchunk) : n;
                if (copyin(current_proc->pagetable, kchunk, ubuf + written, chunk) < 0) {
                    return (written > 0) ? (long)written : (long)-14;
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
                return (total > 0) ? (long)total : (long)-14;
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
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
            /* Check for pending signals after waking up:
             * - User handler (handler > 1): return -EINTR
             * - SIG_DFL terminating signal: break out to let deliver_signal kill us
             * - SIG_IGN / SIG_DFL ignore-class: continue sleeping */
            {
                uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                struct kern_sigaction *sa = proc_sigactions(current_proc);
                for (int s = 1; s < 32 && deliverable; s++) {
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
                return (read_bytes > 0) ? (long)read_bytes : (long)-14;
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
                return (total > 0) ? (long)total : (long)-14;
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
    int flags = (int)tf->a2;

    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }

    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);

    /* Resolve symlinks: intermediate always, final unless O_NOFOLLOW */
    {
        int nofollow = (flags & 0400000); /* O_NOFOLLOW = 0400000 */
        int symret = resolve_symlinks_in_path(resolved, 8, !nofollow);
        if (symret == -40)
            return (long)-40; /* -ELOOP */
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
#define O_CREAT    0100
#define O_TRUNC    01000
#define O_DIRECTORY 0200000
    {
        int is_dir = vfs_is_dir(resolved);
        /* Reject O_DIRECTORY on a non-directory before any side-effects */
        if ((flags & O_DIRECTORY) && !is_dir)
            return (long)-20; /* -ENOTDIR */
        /* Reject O_WRONLY/O_RDWR on directories */
        if (is_dir) {
            int accmode = flags & 3;
            if (accmode == 1 || accmode == 2) /* O_WRONLY or O_RDWR */
                return (long)-21; /* -EISDIR */
        }
        /* Reject O_CREAT on an existing directory (POSIX: EISDIR) */
        if (is_dir && (flags & O_CREAT))
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
        if (flags & 02000000) /* O_CLOEXEC */
            current_proc->fd_flags[fd] |= 1; /* FD_CLOEXEC */
        if (flags & 040000000) /* O_CLOFORK */
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
            if (flags & 02000000)  /* O_CLOEXEC */
                current_proc->fd_flags[newfd] |= 1; /* FD_CLOEXEC */
            if (flags & 040000000) /* O_CLOFORK */
                current_proc->fd_flags[newfd] |= 2; /* FD_CLOFORK */
        }
        return (long)newfd;
    }
}

long sys_getcwd_handler(struct trapframe *tf) {
    if (tf->a0 == 0) return (long)-14; /* -EFAULT: NULL buffer */
    size_t sz = (size_t)tf->a1;
    const char *cwd = current_proc->cwd;
    if (!cwd[0]) cwd = "/";
    int len = strlen(cwd);
    if ((size_t)(len + 1) > sz) {
        return (long)-34; /* -ERANGE: buffer too small */
    }
    if (copyout(current_proc->pagetable, tf->a0, (void *)cwd, len + 1) < 0) {
        return (long)-14; /* -EFAULT */
    }
    return (long)tf->a0; /* return buf pointer */
}

long sys_chdir_handler(struct trapframe *tf) {
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a0, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    /* Empty path is invalid */
    if (kpath[0] == '\0') return (long)-2; /* -ENOENT */
    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);
    resolve_symlinks_in_path(resolved, 8, 1);
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

long sys_fstat_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f) {
        return (long)-9; /* EBADF */
    }
    if (!tf->a1) {
        return (long)-14; /* EFAULT */
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
        if (copyout(current_proc->pagetable, tf->a1, kstat, 128) < 0) {
            return (long)-14;
        }
        return (long)0;
    } else if (f->ops && f->ops->fstat) {
        uint8_t kstat[128];
        int ret = f->ops->fstat(f->fs_data, f->private, kstat);
        if (ret == 0) {
            if (copyout(current_proc->pagetable, tf->a1, kstat, 128) < 0) {
                return (long)-14;
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
        if (copyout(current_proc->pagetable, tf->a1, kstat, 128) < 0) {
            return (long)-14;
        }
        return (long)0;
    }
}

long sys_newfstatat_handler(struct trapframe *tf) {
    if (!tf->a1 || !tf->a2) {
        return (long)-1;
    }
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);

    /* Resolve symlinks: intermediate always, final unless AT_SYMLINK_NOFOLLOW */
    int at_flags = (int)tf->a3;
    {
        int nofollow = (at_flags & 0x100); /* AT_SYMLINK_NOFOLLOW = 0x100 */
        int symret = resolve_symlinks_in_path(resolved, 8, !nofollow);
        if (symret == -40)
            return (long)-40; /* -ELOOP */
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ops && ops->stat) {
        uint8_t kstat[128];
        int ret = ops->stat(mnt->fs_data, relpath, kstat);
        if (ret == 0) {
            if (copyout(current_proc->pagetable, tf->a2, kstat, 128) < 0) {
                return (long)-14;
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
                    return (long)-14;
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
            return (long)-14;
        }
        int64_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            uint64_t buf_uaddr = kiov[i].base;
            uint64_t len = kiov[i].len;
            if (len == 0) continue;
            if (!buf_uaddr) {
                if (total == 0) total = -14;
                break;
            }
            if (f->is_pipe) {
                int pid = f->pipe_id;
                if (pid < 0 || !f->pipe_write) { total = -9; break; }
                if (pipes[pid].read_open <= 0) {
                    current_proc->sigpending |= signal_bit(SIGPIPE);
                    if (total == 0) total = -32; /* -EPIPE */
                    goto writev_out;
                }
                char kchunk[512];
                uint64_t done = 0;
                while (done < len) {
                    uint64_t clen = len - done;
                    if (clen > sizeof(kchunk)) clen = sizeof(kchunk);
                    if (copyin(current_proc->pagetable, kchunk, buf_uaddr + done, clen) < 0) {
                        if (total == 0) total = -14;
                        goto writev_out;
                    }
                    for (uint64_t j = 0; j < clen; j++) {
                        while (pipe_avail(pid) >= KPIPE_BUF) {
                            if (!pipes[pid].read_open) break;
                            current_proc->state = SLEEPING;
                            swtch(&current_proc->context, &scheduler_context);
                            /* Check for terminating signals after waking */
                            {
                                uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                                struct kern_sigaction *ksa = proc_sigactions(current_proc);
                                for (int s = 1; s < 32 && deliverable; s++) {
                                    if (deliverable & signal_bit(s)) {
                                        if (ksa[s].handler > 1) { /* user handler */
                                            if (total == 0) total = -4; /* -EINTR */
                                            goto writev_out;
                                        }
                                        if (ksa[s].handler == 0) { /* SIG_DFL */
                                            if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                                                continue; /* ignore-class */
                                            if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                                                continue; /* stop-class */
                                            if (s == SIGCONT)
                                                continue; /* resume */
                                            /* Terminating signal: return to let deliver_signal kill */
                                            if (total == 0) total = -4;
                                            goto writev_out;
                                        }
                                    }
                                }
                            }
                        }
                        if (!pipes[pid].read_open) {
                            current_proc->sigpending |= signal_bit(SIGPIPE);
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
                        if (total == 0) total = -14;
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
            return (long)-14;
        }
        int64_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            uint64_t buf_uaddr = kiov[i].base;
            uint64_t len = kiov[i].len;
            if (len == 0) continue;
            if (!buf_uaddr) {
                if (total == 0) total = -14;
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
                        uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                        struct kern_sigaction *ksa = proc_sigactions(current_proc);
                        for (int s = 1; s < 32 && deliverable; s++) {
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
                        if (total == 0) total = -14;
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
                        if (total == 0) total = -14;
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
            return total > 0 ? (long)total : (long)-14;
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
            return total > 0 ? (long)total : (long)-14;
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
            ftab[fd]->flags |= 02000000; /* O_CLOEXEC */
        else
            ftab[fd]->flags &= ~02000000;
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
            return (long)-14; /* EFAULT */
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
            return (long)-14;
        return (long)0;
    } else if (cmd == 6 || cmd == 7 || cmd == 37 || cmd == 38) {
        /* F_SETLK (6) / F_SETLKW (7) / F_OFD_SETLK (37) / F_OFD_SETLKW (38)
         * -- set, upgrade, or release a file lock.  Wait variants also return
         * EAGAIN in this teaching OS (no blocking lock support). */
        struct { short l_type; short l_whence; int64_t l_start;
                 int64_t l_len; pid_t l_pid; } kfl;
        if (copyin(current_proc->pagetable, &kfl, tf->a2, sizeof(kfl)) < 0)
            return (long)-14;
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
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }

    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);
    resolve_symlinks_in_path(resolved, 8, 1);

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
    int flags = (int)tf->a2;
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    {
        char resolved[128];
        resolve_path(current_proc->cwd, kpath, resolved);
        /* Resolve intermediate symlinks; do NOT follow final (unlink targets the link itself) */
        resolve_symlinks_in_path(resolved, 8, 0);
        const char *relpath;
        struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
        struct vfs_ops *ops = mnt ? mnt->ops : NULL;
        if (flags & 0x200) {
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
    char kold[128], knew[256];
    if (copyinstr(current_proc->pagetable, kold, tf->a1, sizeof(kold)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, knew, tf->a3, sizeof(knew)) < 0)
        return (long)-14;

    char old_resolved[128], new_resolved[256];
    resolve_path(current_proc->cwd, kold, old_resolved);
    resolve_path(current_proc->cwd, knew, new_resolved);

    /* Both paths must be on ext2 (same filesystem) */
    const char *old_rel, *new_rel;
    struct mount *old_mnt = vfs_lookup_mount_entry(old_resolved, &old_rel);
    struct mount *new_mnt = vfs_lookup_mount_entry(new_resolved, &new_rel);
    struct vfs_ops *old_ops = old_mnt ? old_mnt->ops : NULL;
    struct vfs_ops *new_ops = new_mnt ? new_mnt->ops : NULL;
    if (old_ops != new_ops ||
        (old_mnt && new_mnt && old_mnt->fs_data != new_mnt->fs_data))
        return (long)-18; /* EXDEV: cross-device link */
    if (!ext2_is_ops(old_ops))
        return (long)-30; /* EROFS: read-only filesystem */

    /* Get the source inode number by opening the old path */
    void *priv = NULL;
    if (old_ops->open(old_mnt->fs_data, old_rel, 0, &priv) < 0)
        return (long)-2; /* ENOENT */
    struct ext2_open_private *ep = (struct ext2_open_private *)priv;
    uint32_t src_ino = ep->ino;
    if (old_ops->close) old_ops->close(old_mnt->fs_data, priv);

    /* Create a new directory entry pointing to the same inode.
     * Use ext2_mknod-style approach: create entry in parent dir + increment link_count.
     * For simplicity, use the kernel's ext2 internal functions via a helper. */
    int ret = ext2_hardlink(new_mnt->fs_data, new_rel, src_ino);
    return (long)ret;
}

long sys_renameat2_handler(struct trapframe *tf) {
    char kold[128], knew[256];
    if (copyinstr(current_proc->pagetable, kold, tf->a1, sizeof(kold)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, knew, tf->a3, sizeof(knew)) < 0)
        return (long)-14;

    char old_resolved[128], new_resolved[256];
    resolve_path(current_proc->cwd, kold, old_resolved);
    resolve_path(current_proc->cwd, knew, new_resolved);

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
    int mode = (int)tf->a2;
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    {
        char resolved[128];
        resolve_path(current_proc->cwd, kpath, resolved);
        /* Resolve intermediate symlinks only (creating at final component) */
        resolve_symlinks_in_path(resolved, 8, 0);
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
    uint32_t mode = (uint32_t)tf->a2;
    uint32_t rdev = (uint32_t)tf->a3;
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    {
        char resolved[128];
        resolve_path(current_proc->cwd, kpath, resolved);
        const char *relpath;
        struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
        struct vfs_ops *ops = mnt ? mnt->ops : NULL;
        if (ext2_is_ops(ops)) {
            int ret = ext2_mknod(mnt->fs_data, relpath, mode, rdev);
            return (long)(int64_t)ret;
        } else {
            return (long)-30;
        }
    }
}

long sys_readlinkat_handler(struct trapframe *tf) {
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0)
        return (long)-14;

    int bufsiz = (int)tf->a3;
    if (bufsiz <= 0)
        return (long)-22;

    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops || !ops->readlink)
        return (long)-22; /* EINVAL */

    char kbuf[128];
    int maxread = bufsiz > 256 ? 256 : bufsiz;
    int ret = ops->readlink(mnt->fs_data, relpath, kbuf, maxread);
    if (ret < 0)
        return (long)(int64_t)ret;

    if (copyout(current_proc->pagetable, tf->a2, kbuf, (uint64_t)ret) < 0)
        return (long)-14;
    return (long)ret;
}

long sys_symlinkat_handler(struct trapframe *tf) {
    char ktarget[128], klinkpath[256];
    if (copyinstr(current_proc->pagetable, ktarget, tf->a0, sizeof(ktarget)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, klinkpath, tf->a2, sizeof(klinkpath)) < 0)
        return (long)-14;

    char resolved[128];
    resolve_path(current_proc->cwd, klinkpath, resolved);

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ops && ops->symlink)
        return (long)(int64_t)ops->symlink(mnt->fs_data, ktarget, relpath);
    return (long)-30; /* EROFS */
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
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0)
        return (long)-14; /* EFAULT */

    /* Resolve path and verify it exists via stat */
    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);

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
