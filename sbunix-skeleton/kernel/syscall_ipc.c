/* RF: IPC syscall handlers and pipe infrastructure extracted from trap.c.
 * Pipes, poll, futex, epoll, timerfd, inotify. */

#include <syscall_handlers.h>
#include <trap_internal.h>
#include <pipe.h>
#include <proc.h>
#include <vfs.h>
#include <riscv.h>
#include <string.h>
#include <printk.h>
#include <kmalloc.h>
#include <vm.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* Kernel pipe table                                                    */
/* RF: struct kpipe, NPIPES, KPIPE_BUF defined in pipe.h.              */
/* pipes[] array, alloc_pipe, pipe_avail moved from trap.c.            */
/* ------------------------------------------------------------------ */

struct kpipe pipes[NPIPES];

int alloc_pipe(void) {
  for (int i = 0; i < NPIPES; i++) {
    if (!pipes[i].used) {
      pipes[i].used = 1;
      pipes[i].rpos = 0;
      pipes[i].wpos = 0;
      pipes[i].write_open = 1;
      pipes[i].read_open = 1;
      return i;
    }
  }
  return -1;
}

/* RF: Made non-static for access by extracted IPC module */
int pipe_avail(int pid) {
  return pipes[pid].wpos - pipes[pid].rpos;
}

/* ------------------------------------------------------------------ */
/* Poll readiness helper                                                */
/* RF: Moved from trap.c -- used by ppoll, pselect6, epoll handlers.   */
/* ------------------------------------------------------------------ */

short check_fd_ready(struct open_file *f, short events) {
    if (!f)
        return KPOLLNVAL;

    short revents = 0;

    if (f->is_pipe) {
        int pid = f->pipe_id;
        if (pid < 0 || !pipes[pid].used)
            return KPOLLNVAL;

        if (f->pipe_read) {
            if (events & KPOLLIN) {
                if (pipe_avail(pid) > 0)
                    revents |= KPOLLIN;
            }
            /* POLLHUP: write end closed and no data left */
            if (pipes[pid].write_open <= 0 && pipe_avail(pid) == 0)
                revents |= KPOLLHUP;
        }
        if (f->pipe_write) {
            if (events & KPOLLOUT) {
                if (pipe_avail(pid) < KPIPE_BUF)
                    revents |= KPOLLOUT;
            }
            /* POLLERR: read end closed */
            if (pipes[pid].read_open <= 0)
                revents |= KPOLLERR;
        }
    } else {
        /* Regular file or device: always ready */
        revents = events & (KPOLLIN | KPOLLOUT);
    }

    return revents;
}

/* ------------------------------------------------------------------ */
/* sys_pipe2 handler                                                    */
/* RF: Moved from trap.c.                                              */
/* ------------------------------------------------------------------ */

long sys_pipe2_handler(struct trapframe *tf) {
    int pipe_flags = (int)tf->a1;
    int pipeid = alloc_pipe();
    if (pipeid < 0) {
        return (long)-1;
    } else {
        struct open_file **ftab = proc_ofile(current_proc);
        struct open_file *rf = vfs_alloc_file();
        struct open_file *wf = vfs_alloc_file();
        int rfd = -1, wfd = -1;
        if (rf) rfd = alloc_fd();
        if (rfd >= 0 && rf) {
            rf->is_pipe = 1;
            rf->pipe_read = 1;
            rf->pipe_write = 0;
            rf->pipe_id = pipeid;
            rf->ops = NULL;
            /* BF: Preserve pipe status flags so read/write can honor
             * O_NONBLOCK in addition to descriptor flags. */
            rf->flags = pipe_flags & (02000000 | 04000); /* O_CLOEXEC | O_NONBLOCK */
            ftab[rfd] = rf;
            if (wf) wfd = alloc_fd();
        }
        if (wfd < 0 || !wf) {
            if (rfd >= 0) {
                ftab[rfd] = NULL;
                vfs_free_file(rf);
            } else if (rf) {
                vfs_free_file(rf);
            }
            if (wf) vfs_free_file(wf);
            pipes[pipeid].used = 0;
            return (long)-1;
        } else {
            wf->is_pipe = 1;
            wf->pipe_read = 0;
            wf->pipe_write = 1;
            wf->pipe_id = pipeid;
            wf->ops = NULL;
            /* BF: Keep O_NONBLOCK on the open-file object, not only fd_flags. */
            wf->flags = 1 | (pipe_flags & (02000000 | 04000)); /* O_WRONLY + flags */
            ftab[wfd] = wf;
            /* ENH: Set per-fd flags for pipe fds based on pipe_flags.
             * O_CLOEXEC and O_CLOFORK propagate to fd_flags. */
            current_proc->fd_flags[rfd] = 0;
            current_proc->fd_flags[wfd] = 0;
            if (pipe_flags & 02000000) { /* O_CLOEXEC */
                current_proc->fd_flags[rfd] |= 1;
                current_proc->fd_flags[wfd] |= 1;
            }
            if (pipe_flags & 040000000) { /* O_CLOFORK */
                current_proc->fd_flags[rfd] |= 2;
                current_proc->fd_flags[wfd] |= 2;
            }
            int kfds[2] = { rfd, wfd };
            if (copyout(current_proc->pagetable, tf->a0, kfds, sizeof(kfds)) < 0) {
                close_ofile(current_proc, rfd);
                close_ofile(current_proc, wfd);
                return (long)-EFAULT;
            }
            return (long)0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* sys_futex handler                                                    */
/* RF: Moved from trap.c.                                              */
/* ------------------------------------------------------------------ */

/* futex — basic FUTEX_WAIT and FUTEX_WAKE for thread synchronization */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

long sys_futex_handler(struct trapframe *tf) {
    uint64_t uaddr = tf->a0;
    int op = (int)tf->a1 & ~FUTEX_PRIVATE_FLAG; /* strip FUTEX_PRIVATE */
    uint32_t val = (uint32_t)tf->a2;

    if (op == FUTEX_WAIT) {
        /* Read the current value at uaddr */
        uint32_t curval;
        if (copyin(current_proc->pagetable, &curval, uaddr, sizeof(curval)) < 0)
            return (long)-EFAULT; /* EFAULT */
        /* If value changed, return EAGAIN */
        if (curval != val)
            return (long)-11; /* EAGAIN */
        /* Sleep on this address */
        current_proc->futex_addr = uaddr;
        current_proc->state = SLEEPING;
        swtch(&current_proc->context, &scheduler_context);
        current_proc->futex_addr = 0;
        /* Check for signal interruption */
        if (current_proc->sigpending & ~current_proc->sigmask) {
            struct kern_sigaction *ksa = proc_sigactions(current_proc);
            for (int s = 1; s < 65; s++) {
                uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                if (deliverable & signal_bit(s)) {
                    if (ksa[s].handler > 1) /* user handler */
                        return (long)-4; /* EINTR */
                    if (ksa[s].handler == 0) { /* SIG_DFL */
                        if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                            continue;
                        if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                            continue;
                        if (s == SIGCONT)
                            continue;
                        /* Terminating: return to let deliver_signal kill */
                        return (long)-4;
                    }
                }
            }
        }
        return 0;
    } else if (op == FUTEX_WAKE) {
        /* Wake up to val processes sleeping on uaddr */
        int woken = 0;
        uint32_t max_wake = val;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->state == SLEEPING && p->futex_addr == uaddr) {
                p->state = RUNNABLE;
                p->futex_addr = 0;
                woken++;
                if ((uint32_t)woken >= max_wake)
                    break;
            }
        }
        return (long)woken;
    }
    return (long)-22; /* EINVAL: unsupported futex op */
}

/* ------------------------------------------------------------------ */
/* epoll — I/O event notification                                       */
/* RF: Moved from trap.c.                                              */
/* ------------------------------------------------------------------ */

#define MAX_EPOLL_ENTRIES 64
struct epoll_entry {
    int fd;
    uint32_t events;
    uint64_t data;
};
struct epoll_private {
    struct epoll_entry entries[MAX_EPOLL_ENTRIES];
    int count;
};

long sys_epoll_create1_handler(struct trapframe *tf) {
    (void)tf;
    struct open_file **ftab = proc_ofile(current_proc);
    int efd = alloc_fd();
    if (efd < 0) return (long)-24; /* EMFILE */
    struct open_file *f = vfs_alloc_file();
    if (!f) return (long)-24;
    f->is_pipe = 0;
    f->ops = NULL;
    f->flags = 0;
    /* Allocate epoll private data */
    struct epoll_private *ep = (struct epoll_private *)kmalloc(sizeof(struct epoll_private));
    if (!ep) { vfs_free_file(f); return (long)-12; }
    memset(ep, 0, sizeof(*ep));
    f->private = ep;
    ftab[efd] = f;
    return (long)efd;
}

long sys_epoll_ctl_handler(struct trapframe *tf) {
    int epfd = (int)tf->a0;
    int op = (int)tf->a1;
    int fd = (int)tf->a2;
    uint64_t event_uva = tf->a3;
    struct open_file **ftab = proc_ofile(current_proc);
    if (epfd < 0 || epfd >= NOFILE || !ftab[epfd]) return (long)-9;
    struct epoll_private *ep = (struct epoll_private *)ftab[epfd]->private;
    if (!ep) return (long)-22;
    /* Read epoll_event from user: {uint32_t events, uint64_t data} = 12 bytes */
    uint8_t kevt[16];
    if (event_uva && copyin(current_proc->pagetable, kevt, event_uva, 12) < 0)
        return (long)-EFAULT;
    uint32_t events = 0;
    uint64_t data = 0;
    if (event_uva) {
        memcpy(&events, kevt, 4);
        memcpy(&data, kevt + 4, 8);
    }
    if (op == 1) { /* EPOLL_CTL_ADD */
        if (ep->count >= MAX_EPOLL_ENTRIES) return (long)-28; /* ENOSPC */
        ep->entries[ep->count].fd = fd;
        ep->entries[ep->count].events = events;
        ep->entries[ep->count].data = data;
        ep->count++;
    } else if (op == 2) { /* EPOLL_CTL_DEL */
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) {
                ep->entries[i] = ep->entries[ep->count - 1];
                ep->count--;
                break;
            }
        }
    } else if (op == 3) { /* EPOLL_CTL_MOD */
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) {
                ep->entries[i].events = events;
                ep->entries[i].data = data;
                break;
            }
        }
    }
    return 0;
}

long sys_epoll_pwait_handler(struct trapframe *tf) {
    int epfd = (int)tf->a0;
    uint64_t events_uva = tf->a1;
    int maxevents = (int)tf->a2;
    int timeout_ms = (int)tf->a3;
    struct open_file **ftab = proc_ofile(current_proc);
    if (epfd < 0 || epfd >= NOFILE || !ftab[epfd]) return (long)-9;
    struct epoll_private *ep = (struct epoll_private *)ftab[epfd]->private;
    if (!ep) return (long)-22;
    /* Check readiness of each registered fd */
    int nready = 0;
    for (int i = 0; i < ep->count && nready < maxevents; i++) {
        int fd = ep->entries[i].fd;
        if (fd < 0 || fd >= NOFILE || !ftab[fd]) continue;
        struct open_file *f = ftab[fd];
        int ready = 0;
        if (f->is_pipe && f->pipe_read) {
            int pid = f->pipe_id;
            if (pipe_avail(pid) > 0 || pipes[pid].write_open <= 0) ready = 1;
        } else if (f->is_pipe && f->pipe_write) {
            int pid = f->pipe_id;
            if (pipe_avail(pid) < KPIPE_BUF) ready = 1;
        } else {
            ready = 1; /* regular files are always ready */
        }
        if (ready) {
            uint8_t kevt[12];
            uint32_t revents = ep->entries[i].events & 0x01; /* EPOLLIN */
            if (f->is_pipe && f->pipe_write) revents = ep->entries[i].events & 0x04; /* EPOLLOUT */
            memcpy(kevt, &revents, 4);
            memcpy(kevt + 4, &ep->entries[i].data, 8);
            if (copyout(current_proc->pagetable, events_uva + (uint64_t)nready * 12, kevt, 12) < 0)
                return (long)-EFAULT;
            nready++;
        }
    }
    if (nready == 0 && timeout_ms != 0) {
        /* Sleep and retry once */
        current_proc->state = SLEEPING;
        swtch(&current_proc->context, &scheduler_context);
        /* After wake, re-check (simplified: return 0 if still nothing) */
        return 0;
    }
    return (long)nready;
}

/* ------------------------------------------------------------------ */
/* timerfd — timer that delivers expiry via file descriptor              */
/* RF: Moved from trap.c.                                              */
/* ------------------------------------------------------------------ */

struct timerfd_private {
    uint64_t expiry_ticks; /* absolute tick count when timer fires (0=disarmed) */
    uint64_t interval;     /* for repeating timers (ticks between expirations) */
    uint64_t expirations;  /* count of expirations since last read */
};

long sys_timerfd_create_handler(struct trapframe *tf) {
    (void)tf;
    struct open_file **ftab = proc_ofile(current_proc);
    int tfd = alloc_fd();
    if (tfd < 0) return (long)-24;
    struct open_file *f = vfs_alloc_file();
    if (!f) return (long)-24;
    f->is_pipe = 0;
    f->ops = NULL;
    f->flags = 0;
    struct timerfd_private *tp = (struct timerfd_private *)kmalloc(sizeof(struct timerfd_private));
    if (!tp) { vfs_free_file(f); return (long)-12; }
    memset(tp, 0, sizeof(*tp));
    f->private = tp;
    ftab[tfd] = f;
    return (long)tfd;
}

long sys_timerfd_settime_handler(struct trapframe *tf) {
    int tfd = (int)tf->a0;
    uint64_t new_uva = tf->a2;
    struct open_file **ftab = proc_ofile(current_proc);
    if (tfd < 0 || tfd >= NOFILE || !ftab[tfd]) return (long)-9;
    struct timerfd_private *tp = (struct timerfd_private *)ftab[tfd]->private;
    if (!tp) return (long)-22;
    /* Read struct itimerspec: {interval{sec,nsec}, value{sec,nsec}} = 32 bytes */
    uint8_t kits[32];
    if (copyin(current_proc->pagetable, kits, new_uva, 32) < 0) return (long)-EFAULT;
    int64_t val_sec, val_nsec, int_sec, int_nsec;
    memcpy(&int_sec, kits, 8);
    memcpy(&int_nsec, kits + 8, 8);
    memcpy(&val_sec, kits + 16, 8);
    memcpy(&val_nsec, kits + 24, 8);
    if (int_nsec < 0 || int_nsec >= (int64_t)NSEC_PER_SEC ||
        val_nsec < 0 || val_nsec >= (int64_t)NSEC_PER_SEC ||
        int_sec < 0 || val_sec < 0)
        return (long)-EINVAL;
    uint64_t now = r_time();
    if (val_sec > 0 || val_nsec > 0) {
        tp->expiry_ticks = now + timespec_to_ticks((uint64_t)val_sec,
                                                   (uint64_t)val_nsec);
    } else {
        tp->expiry_ticks = 0; /* disarm */
    }
    tp->interval = timespec_to_ticks((uint64_t)int_sec, (uint64_t)int_nsec);
    tp->expirations = 0;
    return 0;
}

long sys_timerfd_gettime_handler(struct trapframe *tf) {
    int tfd = (int)tf->a0;
    uint64_t out_uva = tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (tfd < 0 || tfd >= NOFILE || !ftab[tfd]) return (long)-9;
    struct timerfd_private *tp = (struct timerfd_private *)ftab[tfd]->private;
    if (!tp) return (long)-22;
    uint8_t kits[32];
    memset(kits, 0, 32);
    int64_t int_sec;
    int64_t int_nsec;
    ticks_to_timespec(tp->interval, &int_sec, &int_nsec);
    memcpy(kits, &int_sec, 8);
    memcpy(kits + 8, &int_nsec, 8);
    if (tp->expiry_ticks > 0) {
        uint64_t now = r_time();
        int64_t remaining = (int64_t)(tp->expiry_ticks - now);
        if (remaining < 0) remaining = 0;
        int64_t rem_sec;
        int64_t rem_nsec;
        ticks_to_timespec((uint64_t)remaining, &rem_sec, &rem_nsec);
        memcpy(kits + 16, &rem_sec, 8);
        memcpy(kits + 24, &rem_nsec, 8);
    }
    if (copyout(current_proc->pagetable, out_uva, kits, 32) < 0) return (long)-EFAULT;
    return 0;
}

/* ------------------------------------------------------------------ */
/* inotify — filesystem event monitoring                                */
/* RF: Moved from trap.c.                                              */
/* ------------------------------------------------------------------ */

#define MAX_INOTIFY_WATCHES 32
struct inotify_watch {
    char path[128];
    uint32_t mask;
    int wd; /* watch descriptor */
};
struct inotify_private {
    struct inotify_watch watches[MAX_INOTIFY_WATCHES];
    int count;
    int next_wd;
};

long sys_inotify_init1_handler(struct trapframe *tf) {
    (void)tf;
    struct open_file **ftab = proc_ofile(current_proc);
    int ifd = alloc_fd();
    if (ifd < 0) return (long)-24;
    struct open_file *f = vfs_alloc_file();
    if (!f) return (long)-24;
    f->is_pipe = 0;
    f->ops = NULL;
    f->flags = 0;
    struct inotify_private *ip = (struct inotify_private *)kmalloc(sizeof(struct inotify_private));
    if (!ip) { vfs_free_file(f); return (long)-12; }
    memset(ip, 0, sizeof(*ip));
    ip->next_wd = 1;
    f->private = ip;
    ftab[ifd] = f;
    return (long)ifd;
}

long sys_inotify_add_watch_handler(struct trapframe *tf) {
    int ifd = (int)tf->a0;
    uint64_t path_uva = tf->a1;
    uint32_t mask = (uint32_t)tf->a2;
    struct open_file **ftab = proc_ofile(current_proc);
    if (ifd < 0 || ifd >= NOFILE || !ftab[ifd]) return (long)-9;
    struct inotify_private *ip = (struct inotify_private *)ftab[ifd]->private;
    if (!ip || ip->count >= MAX_INOTIFY_WATCHES) return (long)-28;
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, path_uva, sizeof(kpath)) < 0)
        return (long)-EFAULT;
    int wd = ip->next_wd++;
    struct inotify_watch *w = &ip->watches[ip->count++];
    int len = strlen(kpath);
    if (len >= 127) len = 127;
    memcpy(w->path, kpath, len);
    w->path[len] = '\0';
    w->mask = mask;
    w->wd = wd;
    return (long)wd;
}

long sys_inotify_rm_watch_handler(struct trapframe *tf) {
    int ifd = (int)tf->a0;
    int wd = (int)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (ifd < 0 || ifd >= NOFILE || !ftab[ifd]) return (long)-9;
    struct inotify_private *ip = (struct inotify_private *)ftab[ifd]->private;
    if (!ip) return (long)-22;
    for (int i = 0; i < ip->count; i++) {
        if (ip->watches[i].wd == wd) {
            ip->watches[i] = ip->watches[ip->count - 1];
            ip->count--;
            return 0;
        }
    }
    return (long)-22; /* EINVAL */
}

/* ------------------------------------------------------------------ */
/* sys_ppoll handler                                                    */
/* RF: Moved from trap.c.                                              */
/* ------------------------------------------------------------------ */

long sys_ppoll_handler(struct trapframe *tf) {
    uint64_t fds_uaddr = tf->a0;
    int nfds = (int)tf->a1;
    uint64_t timeout_uaddr = tf->a2;
    uint64_t sigmask_uaddr = tf->a3;

    if (nfds < 0 || nfds > NOFILE)
        return (long)-22;

    current_proc->has_saved_sigmask = 0;

    /* Copyin pollfd array: each entry is {int fd; short events; short revents} = 8 bytes */
    uint8_t kfds[NOFILE * 8];
    int fds_size = nfds * 8;
    if (fds_size > 0 && copyin(current_proc->pagetable, kfds, fds_uaddr, (uint64_t)fds_size) < 0)
        return (long)-EFAULT;

    /* Handle sigmask swap */
    uint64_t old_mask = current_proc->sigmask;
    /* BF: Snapshot signal_handled_epoch BEFORE mask swap so we can detect
     * signals handled between raise() and the ppoll sleep.  If epoch
     * changed after wakeup, a signal was already delivered and we must
     * return -EINTR. */
    uint64_t epoch_before = current_proc->signal_handled_epoch;
    if (sigmask_uaddr != 0) {
        uint64_t new_mask;
        if (copyin(current_proc->pagetable, &new_mask, sigmask_uaddr, 8) < 0)
            return (long)-EFAULT;
        current_proc->sigmask = new_mask;
    }

    /* BF: POSIX requires ppoll to atomically replace the mask and check
     * for deliverable signals.  If a signal was pending but blocked, and
     * the new mask unblocks it, ppoll must return -EINTR immediately
     * without entering the poll loop. */
    if (current_proc->sigpending & ~current_proc->sigmask) {
        if (sigmask_uaddr != 0) {
            /* BF: Defer mask restore so deliver_signal (at trap return) can
             * deliver the handler under the ppoll-swapped mask.  sigreturn
             * will restore old_mask from the signal frame. */
            current_proc->saved_sigmask_restore = old_mask;
            current_proc->has_saved_sigmask = 1;
        }
        if (nfds > 0 && fds_size > 0)
            copyout(current_proc->pagetable, fds_uaddr, (void *)kfds, (uint64_t)fds_size);
        /* BF: POSIX lists ppoll as a non-restartable syscall.  Clear
         * in_syscall so deliver_signal does not rewind epc when
         * SA_RESTART is set on the handler. */
        current_proc->in_syscall = 0;
        return (long)-4; /* EINTR */
    }

    /* Parse timeout */
    int has_timeout = (timeout_uaddr != 0);
    uint64_t sleep_until_val = 0;
    if (has_timeout) {
        uint64_t kts[2];
        if (copyin(current_proc->pagetable, kts, timeout_uaddr, 16) < 0) {
            current_proc->sigmask = old_mask;
            return (long)-EFAULT;
        }
        if (kts[0] == 0 && kts[1] == 0) {
            /* Non-blocking poll */
        } else {
            if (kts[1] >= 1000000000UL) {
                current_proc->sigmask = old_mask;
                return (long)-22;
            }
            sleep_until_val = timespec_rel_deadline(kts[0], kts[1]);
        }
    }

    struct open_file **ftab = proc_ofile(current_proc);

    /* Check readiness */
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        int fd = *(int *)(kfds + i * 8);
        short events = *(short *)(kfds + i * 8 + 4);
        struct open_file *f = (fd >= 0 && fd < NOFILE) ? ftab[fd] : 0;
        short rev = check_fd_ready(f, events);
        *(short *)(kfds + i * 8 + 6) = rev;
        if (rev)
            ready++;
    }

    /* If none ready and timeout allows blocking */
    if (ready == 0 && (!has_timeout || sleep_until_val > r_time())) {
        /* BF: When !has_timeout (NULL timeout = infinite wait), use
         * waiting_for_uart=1 to block indefinitely.  Previously
         * sleep_until_val stayed 0 and the guard skipped swtch(),
         * causing ppoll to return immediately instead of blocking. */
        if (has_timeout && sleep_until_val > 0) {
            current_proc->sleep_until = sleep_until_val;
            current_proc->waiting_for_uart = 2;
        } else if (!has_timeout) {
            current_proc->waiting_for_uart = 1; /* block indefinitely */
        }
        if (current_proc->waiting_for_uart) {
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
            /* BF: Clear on ALL post-sleep paths — stale waiting_for_uart
             * value (1 or 2) causes scheduler to mistrack process state
             * in subsequent syscalls. */
            current_proc->waiting_for_uart = 0;
        }

        /* BF: ppoll must return -EINTR when a deliverable signal is pending
         * during the wait.  POSIX: "If ... a signal is caught, ppoll() shall
         * be interrupted... and shall return -1 with errno set to [EINTR]."
         * Also check epoch: if a signal was handled between the mask swap
         * and entering sleep, we must still return -EINTR even if
         * sigpending is now clear. */
        if ((current_proc->sigpending & ~current_proc->sigmask) ||
            current_proc->signal_handled_epoch != epoch_before) {
            if (sigmask_uaddr != 0) {
                /* BF: Defer mask restore -- same as early EINTR path above. */
                current_proc->saved_sigmask_restore = old_mask;
                current_proc->has_saved_sigmask = 1;
            }
            if (fds_size > 0)
                copyout(current_proc->pagetable, fds_uaddr, kfds, (uint64_t)fds_size);
            /* BF: POSIX lists ppoll as a non-restartable syscall.  Clear
             * in_syscall so deliver_signal does not rewind epc when
             * SA_RESTART is set on the handler. */
            current_proc->in_syscall = 0;
            return (long)-4; /* EINTR */
        }

        /* Re-check after wakeup */
        ready = 0;
        for (int i = 0; i < nfds; i++) {
            int fd = *(int *)(kfds + i * 8);
            short events = *(short *)(kfds + i * 8 + 4);
            struct open_file *f = (fd >= 0 && fd < NOFILE) ? ftab[fd] : 0;
            short rev = check_fd_ready(f, events);
            *(short *)(kfds + i * 8 + 6) = rev;
            if (rev)
                ready++;
        }
    }

    /* Restore sigmask */
    if (sigmask_uaddr != 0)
        current_proc->sigmask = old_mask;

    /* Copyout results */
    if (fds_size > 0 && copyout(current_proc->pagetable, fds_uaddr, kfds, (uint64_t)fds_size) < 0)
        return (long)-EFAULT;

    return (long)ready;
}

/* ------------------------------------------------------------------ */
/* sys_pselect6 handler                                                 */
/* RF: Moved from trap.c.                                              */
/* ------------------------------------------------------------------ */

long sys_pselect6_handler(struct trapframe *tf) {
    int nfds = (int)tf->a0;
    uint64_t readfds_uaddr = tf->a1;
    uint64_t writefds_uaddr = tf->a2;
    uint64_t exceptfds_uaddr = tf->a3;
    uint64_t timeout_uaddr = tf->a4;

    if (nfds < 0 || nfds > NOFILE)
        nfds = NOFILE;

    current_proc->has_saved_sigmask = 0;

    /* BF: Handle sigmask from a5 (pselect6 ABI: a5 points to {sigmask_ptr, size}).
     * This enables signal-driven interruption and SA_RESTART for select(). */
    uint64_t sigmask_uaddr_raw = tf->a5;
    uint64_t old_mask = current_proc->sigmask;
    /* BF: Snapshot epoch before mask swap to detect signals handled
     * between the swap and sleep (same race as ppoll). */
    uint64_t epoch_before = current_proc->signal_handled_epoch;
    int has_sigmask = 0;
    if (sigmask_uaddr_raw != 0) {
        uint64_t sig_args[2]; /* {sigmask_ptr, sigsetsize} */
        if (copyin(current_proc->pagetable, sig_args, sigmask_uaddr_raw, 16) == 0 &&
            sig_args[0] != 0) {
            uint64_t new_mask;
            if (copyin(current_proc->pagetable, &new_mask, sig_args[0], 8) == 0) {
                current_proc->sigmask = new_mask;
                has_sigmask = 1;
            }
        }
    }

    /* fd_set is 128 bytes but we only need first 8 bytes (64 fds) */
    uint64_t rfds = 0, wfds = 0, efds = 0;
    if (readfds_uaddr && copyin(current_proc->pagetable, &rfds, readfds_uaddr, 8) < 0) {
        if (has_sigmask) current_proc->sigmask = old_mask;
        return (long)-EFAULT;
    }
    if (writefds_uaddr && copyin(current_proc->pagetable, &wfds, writefds_uaddr, 8) < 0) {
        if (has_sigmask) current_proc->sigmask = old_mask;
        return (long)-EFAULT;
    }
    if (exceptfds_uaddr && copyin(current_proc->pagetable, &efds, exceptfds_uaddr, 8) < 0) {
        if (has_sigmask) current_proc->sigmask = old_mask;
        return (long)-EFAULT;
    }

    /* Parse timeout */
    int has_timeout = (timeout_uaddr != 0);
    uint64_t sleep_until_val = 0;
    if (has_timeout) {
        uint64_t kts[2];
        if (copyin(current_proc->pagetable, kts, timeout_uaddr, 16) < 0) {
            if (has_sigmask) current_proc->sigmask = old_mask;
            return (long)-EFAULT;
        }
        if (kts[0] == 0 && kts[1] == 0) {
            /* Non-blocking */
        } else {
            if (kts[1] >= 1000000000UL) {
                if (has_sigmask) current_proc->sigmask = old_mask;
                return (long)-22;
            }
            sleep_until_val = timespec_rel_deadline(kts[0], kts[1]);
        }
    }

    struct open_file **ftab = proc_ofile(current_proc);

    /* Check readiness */
    uint64_t out_rfds = 0, out_wfds = 0, out_efds = 0;
    int ready = 0;

    for (int fd = 0; fd < nfds; fd++) {
        uint64_t bit = 1UL << fd;
        struct open_file *f = (fd < NOFILE) ? ftab[fd] : 0;

        if (rfds & bit) {
            short rev = check_fd_ready(f, KPOLLIN);
            if (rev & (KPOLLIN | KPOLLHUP | KPOLLERR)) {
                out_rfds |= bit;
                ready++;
            }
        }
        if (wfds & bit) {
            short rev = check_fd_ready(f, KPOLLOUT);
            if (rev & KPOLLOUT) {
                out_wfds |= bit;
                ready++;
            }
        }
        if (efds & bit) {
            short rev = check_fd_ready(f, 0);
            if (rev & (KPOLLERR | KPOLLNVAL)) {
                out_efds |= bit;
                ready++;
            }
        }
    }

    /* BF: Check for pending signals before blocking (POSIX atomicity). */
    if (ready == 0 && (current_proc->sigpending & ~current_proc->sigmask)) {
        if (has_sigmask) {
            current_proc->saved_sigmask_restore = old_mask;
            current_proc->has_saved_sigmask = 1;
        }
        out_rfds = out_wfds = out_efds = 0;
        if (readfds_uaddr) copyout(current_proc->pagetable, readfds_uaddr, &out_rfds, 8);
        if (writefds_uaddr) copyout(current_proc->pagetable, writefds_uaddr, &out_wfds, 8);
        if (exceptfds_uaddr) copyout(current_proc->pagetable, exceptfds_uaddr, &out_efds, 8);
        /* BF: POSIX lists pselect as a non-restartable syscall.  Clear
         * in_syscall so deliver_signal does not rewind epc when
         * SA_RESTART is set on the handler. */
        current_proc->in_syscall = 0;
        return (long)-4; /* EINTR */
    }

    /* Block if none ready and timeout allows */
    if (ready == 0 && (!has_timeout || sleep_until_val > r_time())) {
        /* BF: Fix blocking for NULL timeout -- when !has_timeout,
         * sleep_until_val is 0 so we need waiting_for_uart=1 (indefinite). */
        if (has_timeout && sleep_until_val > 0) {
            current_proc->sleep_until = sleep_until_val;
            current_proc->waiting_for_uart = 2;
        } else if (!has_timeout) {
            current_proc->waiting_for_uart = 1; /* block indefinitely */
        }
        if (current_proc->waiting_for_uart) {
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
            /* BF: Clear on ALL post-sleep paths — stale waiting_for_uart
             * value (1 or 2) causes scheduler to mistrack process state
             * in subsequent syscalls. */
            current_proc->waiting_for_uart = 0;
        }

        /* BF: If a signal handler ran between mask swap and here,
         * return -EINTR even though sigpending is now clear. */
        if (current_proc->signal_handled_epoch != epoch_before) {
            if (has_sigmask) {
                current_proc->saved_sigmask_restore = old_mask;
                current_proc->has_saved_sigmask = 1;
            }
            current_proc->in_syscall = 0;
            return (long)-4;  /* EINTR */
        }

        /* BF: Check for signal after wakeup. */
        if (current_proc->sigpending & ~current_proc->sigmask) {
            if (has_sigmask) {
                current_proc->saved_sigmask_restore = old_mask;
                current_proc->has_saved_sigmask = 1;
            }
            out_rfds = out_wfds = out_efds = 0;
            if (readfds_uaddr) copyout(current_proc->pagetable, readfds_uaddr, &out_rfds, 8);
            if (writefds_uaddr) copyout(current_proc->pagetable, writefds_uaddr, &out_wfds, 8);
            if (exceptfds_uaddr) copyout(current_proc->pagetable, exceptfds_uaddr, &out_efds, 8);
            /* BF: POSIX lists pselect as a non-restartable syscall.  Clear
             * in_syscall so deliver_signal does not rewind epc when
             * SA_RESTART is set on the handler. */
            current_proc->in_syscall = 0;
            return (long)-4; /* EINTR */
        }

        /* Re-check */
        out_rfds = out_wfds = out_efds = 0;
        ready = 0;
        for (int fd = 0; fd < nfds; fd++) {
            uint64_t bit = 1UL << fd;
            struct open_file *f = (fd < NOFILE) ? ftab[fd] : 0;
            if (rfds & bit) {
                short rev = check_fd_ready(f, KPOLLIN);
                if (rev & (KPOLLIN | KPOLLHUP | KPOLLERR)) { out_rfds |= bit; ready++; }
            }
            if (wfds & bit) {
                short rev = check_fd_ready(f, KPOLLOUT);
                if (rev & KPOLLOUT) { out_wfds |= bit; ready++; }
            }
            if (efds & bit) {
                short rev = check_fd_ready(f, 0);
                if (rev & (KPOLLERR | KPOLLNVAL)) { out_efds |= bit; ready++; }
            }
        }
    }

    /* Restore sigmask on normal return */
    if (has_sigmask)
        current_proc->sigmask = old_mask;

    /* Copyout modified fd_sets */
    if (readfds_uaddr && copyout(current_proc->pagetable, readfds_uaddr, &out_rfds, 8) < 0)
        return (long)-EFAULT;
    if (writefds_uaddr && copyout(current_proc->pagetable, writefds_uaddr, &out_wfds, 8) < 0)
        return (long)-EFAULT;
    if (exceptfds_uaddr && copyout(current_proc->pagetable, exceptfds_uaddr, &out_efds, 8) < 0)
        return (long)-EFAULT;

    return (long)ready;
}
