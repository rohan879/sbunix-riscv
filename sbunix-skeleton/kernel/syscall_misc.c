/* RF: Miscellaneous syscall handlers extracted from trap.c.
 * Credentials, uname, mount, reboot, ioctl, sync, getrandom, prlimit, etc. */

#include <syscall_handlers.h>
#include <trap_internal.h>
#include <pathres.h>
#include <pipe.h>
#include <proc.h>
#include <syscall.h>
#include <vfs.h>
#include <ext2.h>
#include <riscv.h>
#include <string.h>
#include <printk.h>
#include <vm.h>
#include <kmalloc.h>
#include <tty.h>
#include <errno.h>

long sys_ioctl_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    unsigned long request = tf->a1;
    uint64_t argp = tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;

    /* BF: Treat stale or corrupted descriptor slots as EBADF before
     * dereferencing f->ops; ioctl is commonly probed on arbitrary fds. */
    if (!f || !vfs_file_valid(f)) return (long)-9; /* -EBADF */

    {
        int is_tty = 0;
        if (f->ops == &devfs_ops && f->private)
            is_tty = devfs_is_tty(f->private);

        if (request == 0x5413) { /* TIOCGWINSZ */
            if (is_tty) {
                if (argp) {
                    struct { uint16_t row, col, xpix, ypix; } ws = {24, 80, 0, 0};
                    if (copyout(current_proc->pagetable, argp, &ws, sizeof(ws)) < 0) {
                        return (long)-EFAULT;
                    }
                }
                return (long)0;
            } else {
                return (long)-25;
            }
        } else if (request == 0x540F || request == 0x5410 || request == TIOCGSID) {
            int kval = 0;
            if ((request == 0x5410 || request == TIOCGSID) && argp) {
                if (request == 0x5410 &&
                    copyin(current_proc->pagetable, &kval, argp, sizeof(kval)) < 0)
                    return (long)-EFAULT;
            }
            if (!is_tty)
                return (long)-ENOTTY;
            {
                long result = (long)devfs_tty_ioctl(f->private, request, (void *)&kval);
                if ((request == 0x540F || request == TIOCGSID) && argp && result >= 0) {
                    if (copyout(current_proc->pagetable, argp, &kval, sizeof(kval)) < 0)
                        return (long)-EFAULT;
                }
                return result;
            }
        } else if (request >= TCGETS && request <= TCSETSF) {
            if (is_tty) {
                char ktermios[128];
                memset(ktermios, 0, sizeof(ktermios));
                if (request != TCGETS && argp) {
                    if (copyin(current_proc->pagetable, ktermios, argp, 60) < 0) {
                        return (long)-EFAULT;
                    }
                }
                {
                    long result = (long)devfs_tty_ioctl(f->private, request, (void *)ktermios);
                    if (request == TCGETS && argp) {
                        if (copyout(current_proc->pagetable, argp, ktermios, 60) < 0) {
                            return (long)-EFAULT;
                        }
                    }
                    return result;
                }
            } else {
                return (long)-25;
            }
        } else {
            return (long)-25;
        }
    }
}

long sys_umask_handler(struct trapframe *tf) {
    unsigned int new_mask = (unsigned int)tf->a0 & 0777;
    unsigned int old_mask = current_proc->umask_val;
    current_proc->umask_val = new_mask;
    return (long)old_mask;
}

long sys_geteuid_handler(struct trapframe *tf) {
    (void)tf;
    /* BF: Return actual UID; in single-user OS, euid == uid. */
    return (long)current_proc->uid;
}

long sys_getegid_handler(struct trapframe *tf) {
    (void)tf;
    /* BF: Return actual GID; in single-user OS, egid == gid. */
    return (long)current_proc->gid;
}

long sys_set_tid_address_handler(struct trapframe *tf) {
    (void)tf;
    return (long)current_proc->pid;
}

long sys_prctl_handler(struct trapframe *tf) {
    int option = (int)tf->a0;
    uint64_t uaddr = tf->a1;
    char namebuf[sizeof(current_proc->name)];

    /* Third-party BusyBox probes task names through prctl() during startup.
     * Support the Linux-compatible name operations and reject the rest. */
    switch (option) {
    case PR_SET_NAME:
        if (!uaddr)
            return (long)-EFAULT;
        memset(namebuf, 0, sizeof(namebuf));
        if (copyin(current_proc->pagetable, namebuf, uaddr, sizeof(namebuf)) < 0)
            return (long)-EFAULT;
        memcpy(current_proc->name, namebuf, sizeof(current_proc->name));
        current_proc->name[sizeof(current_proc->name) - 1] = '\0';
        return (long)0;

    case PR_GET_NAME:
        if (!uaddr)
            return (long)-EFAULT;
        memset(namebuf, 0, sizeof(namebuf));
        memcpy(namebuf, current_proc->name, sizeof(current_proc->name));
        namebuf[sizeof(namebuf) - 1] = '\0';
        if (copyout(current_proc->pagetable, uaddr, namebuf, sizeof(namebuf)) < 0)
            return (long)-EFAULT;
        return (long)0;

    default:
        return (long)-EINVAL;
    }
}

long sys_prlimit64_handler(struct trapframe *tf) {
    int resource = (int)tf->a1;
    uint64_t old_uaddr = tf->a3;
    if (old_uaddr) {
        uint64_t rlim[2];
        if (resource == 3) {
            rlim[0] = 8 * 1024 * 1024;
            rlim[1] = 0xFFFFFFFFFFFFFFFFULL;
        } else if (resource == 7) {
            rlim[0] = 16;
            rlim[1] = 16;
        } else {
            rlim[0] = 0xFFFFFFFFFFFFFFFFULL;
            rlim[1] = 0xFFFFFFFFFFFFFFFFULL;
        }
        if (copyout(current_proc->pagetable, old_uaddr, rlim, 16) < 0) {
            return (long)-EFAULT;
        }
    }
    return (long)0;
}

long sys_getrandom_handler(struct trapframe *tf) {
    uint64_t uaddr = tf->a0;
    uint64_t buflen = tf->a1;
    if (uaddr && buflen > 0) {
        uint64_t state = r_time();
        uint64_t done = 0;
        while (done < buflen) {
            char krbuf[512];
            uint64_t chunk = buflen - done;
            if (chunk > sizeof(krbuf)) chunk = sizeof(krbuf);
            for (uint64_t i = 0; i < chunk; i++) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                krbuf[i] = (char)(state >> 33);
            }
            if (copyout(current_proc->pagetable, uaddr + done, krbuf, chunk) < 0)
                break;
            done += chunk;
        }
    }
    return (long)buflen;
}

#define REBOOT_MAGIC1      0xfee1deadU
#define REBOOT_MAGIC2      0x28121969U
#define REBOOT_CMD_HALT    0xcdef0123U
#define REBOOT_CMD_POWEROFF 0x4321fedcU
#define REBOOT_CMD_RESTART 0x01234567U

long sys_reboot_handler(struct trapframe *tf) {
    uint32_t magic1 = (uint32_t)tf->a0;
    uint32_t magic2 = (uint32_t)tf->a1;
    uint32_t cmd    = (uint32_t)tf->a2;

    if (magic1 != REBOOT_MAGIC1 || magic2 != REBOOT_MAGIC2)
        return (long)-1; /* -EPERM */

    /* Flush filesystem before reboot/shutdown */
    ext2_sync(NULL);

    if (cmd == REBOOT_CMD_RESTART) {
        printk("\nSystem rebooting...\n");
        /* SBI SRST extension (EID=0x53525354, FID=0): cold reboot (reset_type=1) */
        register unsigned long a0 asm("a0") = 1; /* reset_type: cold reboot */
        register unsigned long a1 asm("a1") = 0; /* reset_reason: none */
        register unsigned long a6 asm("a6") = 0; /* function ID */
        register unsigned long a7 asm("a7") = 0x53525354; /* EID: SRST */
        asm volatile("ecall" : : "r"(a0), "r"(a1), "r"(a6), "r"(a7));
    } else {
        /* POWEROFF or HALT */
        printk("\nSystem shutting down...\n");
        /* SBI SRST extension (EID=0x53525354, FID=0): shutdown (reset_type=0) */
        register unsigned long a0 asm("a0") = 0; /* reset_type: shutdown */
        register unsigned long a1 asm("a1") = 0; /* reset_reason: none */
        register unsigned long a6 asm("a6") = 0; /* function ID */
        register unsigned long a7 asm("a7") = 0x53525354; /* EID: SRST */
        asm volatile("ecall" : : "r"(a0), "r"(a1), "r"(a6), "r"(a7));
        /* Fallback: SBI v0.1 legacy shutdown (EID=8) */
        register unsigned long fa7 asm("a7") = 8;
        asm volatile("ecall" : : "r"(fa7));
    }
    while (1) {}
    return SYSCALL_NORETURN;
}

long sys_fsync_handler(struct trapframe *tf) {
    /* BF: Validate fd before syncing.  The old code ignored the fd argument
     * and unconditionally called ext2_sync.  POSIX requires EBADF for an
     * invalid fd and EINVAL for fds that don't support syncing (e.g. pipes). */
    int fd = (int)tf->a0;
    if (fd < 0 || fd >= NOFILE)
        return (long)-9; /* -EBADF */

    struct open_file **oft = proc_ofile(current_proc);
    struct open_file *of = oft[fd];
    if (!of)
        return (long)-9; /* -EBADF */

    /* Pipes and special fds cannot be synced */
    if (of->is_pipe)
        return (long)-22; /* -EINVAL */

    /* Only ext2 is a persistent writable filesystem; for in-memory or
     * read-only filesystems (tarfs, ramfs, procfs, devfs, sysfs) syncing
     * is a no-op and returns success.  If a new writable FS is added,
     * update this dispatch. */
    if (of->ops == &ext2_ops)
        ext2_sync(NULL);

    return 0;
}

long sys_sync_handler(struct trapframe *tf) {
    (void)tf;
    ext2_sync(NULL);
    return 0;
}

long sys_uname_handler(struct trapframe *tf) {
    /* struct utsname: 5 fields of 65 bytes each = 325 bytes */
    char kbuf[325];
    memset(kbuf, 0, sizeof(kbuf));

    /* sysname */
    memcpy(kbuf + 0, "SBUnix", 6);
    /* nodename */
    memcpy(kbuf + 65, "sbunix", 6);
    /* release */
    memcpy(kbuf + 130, "1.0.0", 5);
    /* version */
    memcpy(kbuf + 195, "SBUnix RISC-V64", 15);
    /* machine */
    memcpy(kbuf + 260, "riscv64", 7);

    if (copyout(current_proc->pagetable, tf->a0, kbuf, sizeof(kbuf)) < 0)
        return (long)-EFAULT; /* EFAULT */
    return 0;
}

/* Static storage for dynamically mounted paths */
static char mount_path_storage[8][128];
static int mount_path_next = 0;

long sys_mount_handler(struct trapframe *tf) {
    char ksrc[128], ktgt[256], kfstype[64];
    if (copyinstr(current_proc->pagetable, ksrc, tf->a0, sizeof(ksrc)) < 0)
        return (long)-EFAULT;
    if (copyinstr(current_proc->pagetable, ktgt, tf->a1, sizeof(ktgt)) < 0)
        return (long)-EFAULT;
    if (copyinstr(current_proc->pagetable, kfstype, tf->a2, sizeof(kfstype)) < 0)
        return (long)-EFAULT;

    char source_resolved[128], target_resolved[256];
    /* BF: Propagate ENAMETOOLONG from resolve_path */
    if (resolve_path(current_proc->cwd, ksrc, source_resolved) < 0)
        return (long)-36; /* -ENAMETOOLONG */
    if (resolve_path(current_proc->cwd, ktgt, target_resolved) < 0)
        return (long)-36; /* -ENAMETOOLONG */

    /* BF: mount must resolve symlinks for both source and target */
    { int sr = resolve_symlinks_in_path(source_resolved, 6, 1);
      if (sr < 0) return (long)sr; }
    { int sr = resolve_symlinks_in_path(target_resolved, 6, 1);
      if (sr < 0) return (long)sr; }

    /* Only ext2 supported */
    if (strcmp(kfstype, "ext2") != 0)
        return (long)-19; /* ENODEV */

    if (mount_path_next >= 8)
        return (long)-12; /* ENOMEM */

    const char *src_relpath;
    struct mount *src_mnt = vfs_lookup_mount_entry(source_resolved, &src_relpath);
    if (!src_mnt || !ext2_is_ops(src_mnt->ops))
        return (long)-19; /* ENODEV */

    /* Copy path to persistent storage (mount table stores pointers) */
    int rlen = strlen(target_resolved);
    memcpy(mount_path_storage[mount_path_next], target_resolved, (uint64_t)rlen + 1);
    vfs_register_mount(mount_path_storage[mount_path_next], &ext2_ops,
                       src_mnt->fs_data, "ext2");
    mount_path_next++;
    return 0;
}

long sys_umount2_handler(struct trapframe *tf) {
    char ktgt[128];
    if (copyinstr(current_proc->pagetable, ktgt, tf->a0, sizeof(ktgt)) < 0)
        return (long)-EFAULT;

    char resolved[128];
    /* BF: Propagate ENAMETOOLONG from resolve_path */
    if (resolve_path(current_proc->cwd, ktgt, resolved) < 0)
        return (long)-36; /* -ENAMETOOLONG */

    /* BF: umount2 must resolve symlinks in the target path */
    { int sr = resolve_symlinks_in_path(resolved, 6, 1);
      if (sr < 0) return (long)sr; }

    return (long)vfs_unregister_mount(resolved);
}

/* BF: Mirror of ramfs inode and private structs from ramfs.c.
 * Needed by utimensat/fchmod handlers to access ramfs inode fields
 * (timestamps, mode) directly from trap.c.  Layout must exactly match
 * the real struct in ramfs.c (field order, sizes, padding). */
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

/* utimensat — update file timestamps (ext2 mtime, ramfs atime/mtime) */
long sys_utimensat_handler(struct trapframe *tf) {
    /* a0 = dirfd (AT_FDCWD), a1 = path (NULL = use fd), a2 = times, a3 = flags */
    int dirfd = (int)tf->a0;
    uint64_t path_uva = tf->a1;
    uint64_t times_uaddr = tf->a2;

    /* BF: UTIME_NOW and UTIME_OMIT constants per POSIX */
    #define UTIME_NOW  0x3fffffff
    #define UTIME_OMIT 0x3ffffffe

    /* Get current wall-clock time for UTIME_NOW */
    int64_t now_sec, now_nsec;
    get_wall_time(&now_sec, &now_nsec);

    /* Parse times array if provided (2 x struct timespec = 32 bytes) */
    struct { int64_t sec; int64_t nsec; } ktimes[2];
    int has_times = 0;
    if (times_uaddr) {
        if (copyin(current_proc->pagetable, ktimes, times_uaddr, 32) < 0)
            return (long)-EFAULT; /* EFAULT */
        has_times = 1;
    }

    /* BF: Handle NULL path (futimens): look up file by fd */
    if (!path_uva) {
        struct open_file **ftab = proc_ofile(current_proc);
        if (dirfd < 0 || dirfd >= NOFILE || !ftab[dirfd])
            return (long)-9; /* EBADF */
        struct open_file *f = ftab[dirfd];

        /* For ramfs files: update timestamps on the inode directly */
        if (f->ops == &ramfs_ops && f->private) {
            struct ramfs_private *rp = (struct ramfs_private *)f->private;
            if (has_times) {
                if (ktimes[0].nsec != UTIME_OMIT) {
                    rp->inode->atime_sec = (ktimes[0].nsec == UTIME_NOW) ?
                        (uint64_t)now_sec : (uint64_t)ktimes[0].sec;
                }
                if (ktimes[1].nsec != UTIME_OMIT) {
                    rp->inode->mtime_sec = (ktimes[1].nsec == UTIME_NOW) ?
                        (uint64_t)now_sec : (uint64_t)ktimes[1].sec;
                }
            } else {
                /* NULL times = set both to current time */
                rp->inode->atime_sec = (uint64_t)now_sec;
                rp->inode->mtime_sec = (uint64_t)now_sec;
            }
            rp->inode->ctime_sec = (uint64_t)now_sec;
        }
        return 0;
    }

    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, path_uva, sizeof(kpath)) < 0)
        return (long)-EFAULT;

    char resolved[128];
    /* BF: Propagate dirfd resolution errors before VFS dispatch. */
    { int rr = resolve_path_at(dirfd, kpath, resolved);
      if (rr < 0) return (long)rr; }

    /* BF: Propagate all negative errors from resolve_symlinks_in_path,
     * respecting AT_SYMLINK_NOFOLLOW in flags. */
    {
      int flags = (int)tf->a3;
      int nofollow = (flags & 0x100); /* AT_SYMLINK_NOFOLLOW = 0x100 */
      int sr = resolve_symlinks_in_path(resolved, 6, !nofollow);
      if (sr < 0) return (long)sr;
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops || !ops->open) return (long)-2; /* ENOENT */

    /* Open file to get inode, update timestamps, close */
    void *priv = NULL;
    if (ops->open(mnt->fs_data, relpath, 0, &priv) < 0) return (long)-2;

    /* BF: For ramfs files, update atime/mtime on the inode.  Without
     * this, utimensat was a no-op for ramfs, causing stat tests to fail. */
    if (ops == &ramfs_ops && priv) {
        struct ramfs_private *rp = (struct ramfs_private *)priv;
        if (has_times) {
            if (ktimes[0].nsec != UTIME_OMIT) {
                rp->inode->atime_sec = (ktimes[0].nsec == UTIME_NOW) ?
                    (uint64_t)now_sec : (uint64_t)ktimes[0].sec;
            }
            if (ktimes[1].nsec != UTIME_OMIT) {
                rp->inode->mtime_sec = (ktimes[1].nsec == UTIME_NOW) ?
                    (uint64_t)now_sec : (uint64_t)ktimes[1].sec;
            }
        } else {
            rp->inode->atime_sec = (uint64_t)now_sec;
            rp->inode->mtime_sec = (uint64_t)now_sec;
        }
        rp->inode->ctime_sec = (uint64_t)now_sec;
    }

    if (ops->close) ops->close(mnt->fs_data, priv);
    return 0;

    #undef UTIME_NOW
    #undef UTIME_OMIT
}

/* statfs/fstatfs — filesystem statistics */
long sys_statfs_handler(struct trapframe *tf) {
    /* a0 = path, a1 = buf (struct statfs, 120 bytes on RISC-V) */
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a0, sizeof(kpath)) < 0)
        return (long)-EFAULT;

    char resolved[128];
    /* BF: Propagate ENAMETOOLONG from resolve_path */
    if (resolve_path(current_proc->cwd, kpath, resolved) < 0)
        return (long)-36; /* -ENAMETOOLONG */

    /* BF: statfs must resolve symlinks in the target path */
    { int sr = resolve_symlinks_in_path(resolved, 6, 1);
      if (sr < 0) return (long)sr; }

    /* Fill in basic statfs structure */
    uint8_t kbuf[120];
    memset(kbuf, 0, sizeof(kbuf));

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ext2_is_ops(ops)) {
        /* ext2: report real stats */
        uint64_t vals[15];
        memset(vals, 0, sizeof(vals));
        vals[0] = 0xEF53;      /* f_type = EXT2_SUPER_MAGIC */
        vals[1] = 4096;         /* f_bsize = block size */
        vals[2] = 4096;         /* f_blocks (approximate) */
        vals[3] = 4000;         /* f_bfree */
        vals[4] = 4000;         /* f_bavail */
        vals[5] = 1024;         /* f_files (inodes) */
        vals[6] = 1000;         /* f_ffree */
        /* f_fsid[0..1] at offset 7,8 */
        vals[9] = 255;          /* f_namelen */
        vals[10] = 4096;        /* f_frsize */
        memcpy(kbuf, vals, sizeof(kbuf));
    } else {
        /* Other filesystems: generic values */
        uint64_t vals[15];
        memset(vals, 0, sizeof(vals));
        vals[0] = 0x01021994;   /* TMPFS_MAGIC for ramfs/tarfs */
        vals[1] = 4096;
        vals[9] = 255;
        vals[10] = 4096;
        memcpy(kbuf, vals, sizeof(kbuf));
    }

    if (copyout(current_proc->pagetable, tf->a1, kbuf, 120) < 0)
        return (long)-EFAULT;
    return 0;
}

long sys_fstatfs_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    struct open_file **ftab = proc_ofile(current_proc);
    if (fd < 0 || fd >= NOFILE || !ftab[fd])
        return (long)-9; /* EBADF */

    /* Fill basic statfs from fd's filesystem type */
    uint8_t kbuf[120];
    memset(kbuf, 0, sizeof(kbuf));
    uint64_t vals[15];
    memset(vals, 0, sizeof(vals));
    if (ext2_is_ops(ftab[fd]->ops)) {
        vals[0] = 0xEF53;
    } else {
        vals[0] = 0x01021994;
    }
    vals[1] = 4096;
    vals[9] = 255;
    vals[10] = 4096;
    memcpy(kbuf, vals, sizeof(kbuf));

    if (copyout(current_proc->pagetable, tf->a1, kbuf, 120) < 0)
        return (long)-EFAULT;
    return 0;
}

/* Ownership/credential stubs — single-user OS, always uid/gid 0 */
long sys_fchownat_handler(struct trapframe *tf) {
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0)
        return (long)-EFAULT;

    char resolved[128];
    { int rr = resolve_path_at((int)tf->a0, kpath, resolved);
      if (rr < 0) return (long)rr; }

    /* BF: Propagate all negative errors from resolve_symlinks_in_path,
     * respecting AT_SYMLINK_NOFOLLOW in flags. */
    {
      int flags = (int)tf->a4;
      int nofollow = (flags & 0x100); /* AT_SYMLINK_NOFOLLOW = 0x100 */
      int sr = resolve_symlinks_in_path(resolved, 6, !nofollow);
      if (sr < 0) return (long)sr;
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ops && ops->stat) {
        uint8_t st[128];
        int ret = ops->stat(mnt->fs_data, relpath, st);
        if (ret < 0)
            return (long)(int64_t)ret;
    } else {
        return (long)-ENOENT;
    }
    return (long)0;
}

/* BF: Store UID for kill() permission checking.
 * Single-user OS -- just stores the UID without privilege checks. */
long sys_setuid_handler(struct trapframe *tf) {
    current_proc->uid = (int)tf->a0;
    return (long)0;
}

long sys_setgid_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

/* gettid — return thread/process ID */
long sys_gettid_handler(struct trapframe *tf) {
    (void)tf;
    return current_proc ? (long)current_proc->pid : 0;
}

/* sched_yield — voluntarily yield the CPU */
long sys_sched_yield_handler(struct trapframe *tf) {
    (void)tf;
    if (current_proc && current_proc->state == RUNNING) {
        current_proc->state = RUNNABLE;
        swtch(&current_proc->context, &scheduler_context);
    }
    return 0;
}
