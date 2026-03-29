/*
 * procfs.c — /proc virtual filesystem for SBUnix.
 *
 * Exposes per-PID directories with status, cmdline, fd, and maps pseudo-files.
 * Also exposes global files: dmesg, mounts, cpuinfo, version, uptime, meminfo.
 * /proc/self is a magic entry that resolves to /proc/<current_pid>.
 * Read-only; follows the same VFS ops pattern as devfs.c.
 */

#include <vfs.h>
#include <string.h>
#include <printk.h>
#include <kmalloc.h>
#include <proc.h>
#include <kalloc.h>
#include <trap.h>
#include <vm.h>

/* Types of procfs entries */
enum procfs_type {
  PROCFS_ROOT,      /* /proc directory */
  PROCFS_PIDDIR,    /* /proc/<pid> directory */
  PROCFS_STATUS,    /* /proc/<pid>/status */
  PROCFS_CMDLINE,   /* /proc/<pid>/cmdline */
  PROCFS_FDDIR,     /* /proc/<pid>/fd directory */
  PROCFS_MAPS,      /* /proc/<pid>/maps */
  PROCFS_STAT_FILE, /* /proc/<pid>/stat (single-line format) */
  PROCFS_DMESG,     /* /proc/dmesg */
  PROCFS_MOUNTS,    /* /proc/mounts */
  PROCFS_CPUINFO,   /* /proc/cpuinfo */
  PROCFS_VERSION,   /* /proc/version */
  PROCFS_UPTIME,    /* /proc/uptime */
  PROCFS_MEMINFO,   /* /proc/meminfo */
};

/* Buffer size for rendered content */
#define PROCFS_BUF 1024

struct procfs_private {
  enum procfs_type type;
  pid_t pid;
  char buf[PROCFS_BUF];  /* pre-rendered content */
  int len;               /* length of content in buf */
};

/* Helper: write a decimal integer into buf at pos, return new pos */
static int append_int(char *buf, int bufsz, int pos, long v) {
  if (v < 0) {
    if (pos < bufsz) buf[pos++] = '-';
    v = -v;
  }
  char tmp[20];
  int ti = 0;
  if (v == 0) { tmp[ti++] = '0'; }
  else { while (v > 0) { tmp[ti++] = '0' + (int)(v % 10); v /= 10; } }
  while (ti > 0 && pos < bufsz) buf[pos++] = tmp[--ti];
  return pos;
}

/* Helper: write a string into buf */
static int append_str(char *buf, int bufsz, int pos, const char *s) {
  int l = strlen(s);
  if (pos + l > bufsz) l = bufsz - pos;
  if (l > 0) { memcpy(buf + pos, s, l); pos += l; }
  return pos;
}

/* Helper: write a 64-bit value as 16 hex digits (Linux SigPnd/SigBlk format) */
static int append_hex16(char *buf, int bufsz, int pos, uint64_t v) {
  static const char hex[] = "0123456789abcdef";
  for (int i = 15; i >= 0; i--) {
    if (pos < bufsz)
      buf[pos++] = hex[(v >> (i * 4)) & 0xF];
  }
  return pos;
}

/* Helper: parse a decimal PID from a string. Returns -1 on failure. */
static int parse_pid(const char *s) {
  if (!s || !*s) return -1;
  int val = 0;
  while (*s) {
    if (*s < '0' || *s > '9') return -1;
    val = val * 10 + (*s - '0');
    s++;
  }
  return val;
}

/* Helper: find a process by PID */
static struct proc *find_proc(pid_t pid) {
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->pid == pid && p->state != UNUSED)
      return p;
  }
  return 0;
}

/* Helper: state name string */
static const char *state_name(enum procstate s) {
  switch (s) {
    case UNUSED:   return "unused";
    case SLEEPING: return "S (sleeping)";
    case RUNNABLE: return "R (runnable)";
    case RUNNING:  return "R (running)";
    case ZOMBIE:   return "Z (zombie)";
    default:       return "? (unknown)";
  }
}

/* Render /proc/<pid>/status */
static int procfs_render_status(char *buf, int bufsz, struct proc *p) {
  int pos = 0;
  pid_t ppid = p->parent ? p->parent->pid : 0;

  pos = append_str(buf, bufsz, pos, "Name:\t");
  pos = append_str(buf, bufsz, pos, p->name);
  if (pos < bufsz) buf[pos++] = '\n';

  pos = append_str(buf, bufsz, pos, "State:\t");
  pos = append_str(buf, bufsz, pos, state_name(p->state));
  if (pos < bufsz) buf[pos++] = '\n';

  pos = append_str(buf, bufsz, pos, "Pid:\t");
  pos = append_int(buf, bufsz, pos, p->pid);
  if (pos < bufsz) buf[pos++] = '\n';

  pos = append_str(buf, bufsz, pos, "PPid:\t");
  pos = append_int(buf, bufsz, pos, ppid);
  if (pos < bufsz) buf[pos++] = '\n';

  pos = append_str(buf, bufsz, pos, "Uid:\t0\t0\t0\t0\n");
  pos = append_str(buf, bufsz, pos, "Gid:\t0\t0\t0\t0\n");

  pos = append_str(buf, bufsz, pos, "VmSize:\t");
  pos = append_int(buf, bufsz, pos, (long)(p->sz / 1024));
  pos = append_str(buf, bufsz, pos, " kB\n");

  pos = append_str(buf, bufsz, pos, "SigPnd:\t");
  pos = append_hex16(buf, bufsz, pos, p->sigpending);
  if (pos < bufsz) buf[pos++] = '\n';

  pos = append_str(buf, bufsz, pos, "SigBlk:\t");
  pos = append_hex16(buf, bufsz, pos, p->sigmask);
  if (pos < bufsz) buf[pos++] = '\n';

  /* SigIgn and SigCgt for Linux compatibility */
  pos = append_str(buf, bufsz, pos, "SigIgn:\t");
  {
    uint64_t sig_ign = 0;
    struct kern_sigaction *sa = proc_sigactions(p);
    for (int s = 1; s < 32; s++)
      if (sa[s].handler == 1) sig_ign |= signal_bit(s); /* SIG_IGN = 1 */
    pos = append_hex16(buf, bufsz, pos, sig_ign);
  }
  if (pos < bufsz) buf[pos++] = '\n';

  pos = append_str(buf, bufsz, pos, "SigCgt:\t");
  {
    uint64_t sig_cgt = 0;
    struct kern_sigaction *sa = proc_sigactions(p);
    for (int s = 1; s < 32; s++)
      if (sa[s].handler > 1) sig_cgt |= signal_bit(s); /* user handler */
    pos = append_hex16(buf, bufsz, pos, sig_cgt);
  }
  if (pos < bufsz) buf[pos++] = '\n';

  return pos;
}

/* Helper: state character for /proc/<pid>/stat */
static char state_char(enum procstate s) {
  switch (s) {
    case RUNNING:  return 'R';
    case RUNNABLE: return 'R';
    case SLEEPING: return 'S';
    case ZOMBIE:   return 'Z';
    case UNUSED:   return 'U';
    default:       return '?';
  }
}

/* Render /proc/<pid>/stat (single-line format) */
static int procfs_render_stat_file(char *buf, int bufsz, struct proc *p) {
  int pos = 0;
  pid_t ppid = p->parent ? p->parent->pid : 0;

  /* pid */
  pos = append_int(buf, bufsz, pos, p->pid);
  if (pos < bufsz) buf[pos++] = ' ';

  /* (comm) */
  if (pos < bufsz) buf[pos++] = '(';
  pos = append_str(buf, bufsz, pos, p->name);
  if (pos < bufsz) buf[pos++] = ')';
  if (pos < bufsz) buf[pos++] = ' ';

  /* state */
  if (pos < bufsz) buf[pos++] = state_char(p->state);
  if (pos < bufsz) buf[pos++] = ' ';

  /* ppid */
  pos = append_int(buf, bufsz, pos, ppid);
  if (pos < bufsz) buf[pos++] = ' ';

  /* pgid */
  pos = append_int(buf, bufsz, pos, p->pgid);
  if (pos < bufsz) buf[pos++] = ' ';

  /* sid */
  pos = append_int(buf, bufsz, pos, p->sid);
  if (pos < bufsz) buf[pos++] = ' ';

  /* Fields 7-52 (46 fields): tty_nr(7) tpgid(8) flags(9) minflt(10) cminflt(11)
   * majflt(12) cmajflt(13) utime(14) stime(15) cutime(16) cstime(17) priority(18)
   * nice(19) num_threads(20) itrealvalue(21) starttime(22) vsize(23) rss(24)
   * rsslim(25) startcode(26) endcode(27) startstack(28) kstkesp(29) kstkeip(30)
   * signal(31) blocked(32) sigignore(33) sigcatch(34) wchan(35) nswap(36)
   * cnswap(37) exit_signal(38) processor(39) rt_priority(40) policy(41)
   * delayacct_blkio_ticks(42) guest_time(43) cguest_time(44) start_data(45)
   * end_data(46) start_brk(47) arg_start(48) arg_end(49) env_start(50)
   * env_end(51) exit_code(52) */
  pos = append_str(buf, bufsz, pos,
    "0 0 0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 0 ");
  pos = append_int(buf, bufsz, pos, (long)(p->sz)); /* vsize */
  pos = append_str(buf, bufsz, pos,
    " 0 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");

  return pos;
}

/* Render /proc/<pid>/maps (VMA list) */
static int procfs_render_maps(char *buf, int bufsz, struct proc *p) {
  int pos = 0;
  struct vm_area *vma = p->vma_list;
  while (vma && pos < bufsz - 1) {
    /* "start-end rwxp 00000000 00:00 0\n" */
    /* Print start address as hex */
    uint64_t start = vma->start;
    uint64_t end   = vma->end;
    /* Minimal hex printer */
    char hexbuf[17];
    int hi;
    /* start */
    hi = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
      int nibble = (int)((start >> shift) & 0xF);
      if (nibble || hi || shift == 0)
        hexbuf[hi++] = "0123456789abcdef"[nibble];
    }
    hexbuf[hi] = '\0';
    pos = append_str(buf, bufsz, pos, hexbuf);
    if (pos < bufsz) buf[pos++] = '-';
    /* end */
    hi = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
      int nibble = (int)((end >> shift) & 0xF);
      if (nibble || hi || shift == 0)
        hexbuf[hi++] = "0123456789abcdef"[nibble];
    }
    hexbuf[hi] = '\0';
    pos = append_str(buf, bufsz, pos, hexbuf);
    if (pos < bufsz) buf[pos++] = ' ';
    /* permissions */
    if (pos < bufsz) buf[pos++] = (vma->prot & 1) ? 'r' : '-'; /* PROT_READ */
    if (pos < bufsz) buf[pos++] = (vma->prot & 2) ? 'w' : '-'; /* PROT_WRITE */
    if (pos < bufsz) buf[pos++] = (vma->prot & 4) ? 'x' : '-'; /* PROT_EXEC */
    if (pos < bufsz) buf[pos++] = 'p'; /* private */
    pos = append_str(buf, bufsz, pos, " 00000000 00:00 0\n");
    vma = vma->next;
  }
  return pos;
}

/* Render /proc/cpuinfo */
static int procfs_render_cpuinfo(char *buf, int bufsz) {
  int pos = 0;
  pos = append_str(buf, bufsz, pos,
    "processor\t: 0\n"
    "hart\t\t: 0\n"
    "isa\t\t: rv64imac\n"
    "mmu\t\t: sv39\n"
    "uarch\t\t: generic\n"
    "mvendorid\t: 0x0\n"
    "marchid\t\t: 0x0\n"
    "mimpid\t\t: 0x0\n"
  );
  return pos;
}

/* Render /proc/version */
static int procfs_render_version(char *buf, int bufsz) {
  int pos = 0;
  pos = append_str(buf, bufsz, pos,
    "SBUnix version 1.0.0 (SBUnix RISC-V64) #1 SMP\n"
  );
  return pos;
}

/* Render /proc/uptime */
static int procfs_render_uptime(char *buf, int bufsz) {
  int pos = 0;
  unsigned long up = get_uptime_secs();
  pos = append_int(buf, bufsz, pos, (long)up);
  pos = append_str(buf, bufsz, pos, ".00 ");
  pos = append_int(buf, bufsz, pos, (long)up); /* idle = uptime (single CPU) */
  pos = append_str(buf, bufsz, pos, ".00\n");
  return pos;
}

/* Render /proc/meminfo */
static int procfs_render_meminfo(char *buf, int bufsz) {
  int pos = 0;
  /* 128MB total physical memory (PHYSTOP - 0x80000000) */
  long total_kb = (long)(128 * 1024);
  long free_pages = (long)get_free_page_count();
  long free_kb = free_pages * (long)(PAGE_SIZE / 1024);
  long used_kb = total_kb - free_kb;

  pos = append_str(buf, bufsz, pos, "MemTotal:\t");
  pos = append_int(buf, bufsz, pos, total_kb);
  pos = append_str(buf, bufsz, pos, " kB\n");

  pos = append_str(buf, bufsz, pos, "MemFree:\t");
  pos = append_int(buf, bufsz, pos, free_kb);
  pos = append_str(buf, bufsz, pos, " kB\n");

  pos = append_str(buf, bufsz, pos, "MemAvailable:\t");
  pos = append_int(buf, bufsz, pos, free_kb);
  pos = append_str(buf, bufsz, pos, " kB\n");

  pos = append_str(buf, bufsz, pos, "MemUsed:\t");
  pos = append_int(buf, bufsz, pos, used_kb);
  pos = append_str(buf, bufsz, pos, " kB\n");

  pos = append_str(buf, bufsz, pos, "Buffers:\t0 kB\n");
  pos = append_str(buf, bufsz, pos, "Cached:\t\t0 kB\n");
  pos = append_str(buf, bufsz, pos, "SwapTotal:\t0 kB\n");
  pos = append_str(buf, bufsz, pos, "SwapFree:\t0 kB\n");

  return pos;
}

/* ------------------------------------------------------------------ */
/* procfs_open                                                         */
/* ------------------------------------------------------------------ */
static int procfs_open(void *fs_data, const char *path, int flags, void **private) {
  (void)fs_data;
  (void)flags;
  const char *p = path;
  while (*p == '/') p++;

  struct procfs_private *priv = (struct procfs_private *)kmalloc(sizeof(struct procfs_private));
  if (!priv) return -12; /* -ENOMEM */
  memset(priv, 0, sizeof(*priv));

  /* /proc root directory */
  if (*p == '\0') {
    priv->type = PROCFS_ROOT;
    *private = priv;
    return 0;
  }

  /* Global pseudo-files */
  if (strcmp(p, "dmesg") == 0) {
    priv->type = PROCFS_DMESG;
    *private = priv;
    return 0;
  }
  if (strcmp(p, "mounts") == 0) {
    priv->type = PROCFS_MOUNTS;
    int pos = 0;
    const char *mpath, *fstype;
    for (int i = 0; vfs_get_mount_info(i, &mpath, &fstype) == 0; i++) {
      const char *rw = (strcmp(fstype, "ext2") == 0 || strcmp(fstype, "ramfs") == 0) ? "rw" : "ro";
      int fl = strlen(fstype), pl = strlen(mpath), rl = strlen(rw);
      int need = fl + 4 + pl + 2 + rl + 2;
      if (pos + need >= PROCFS_BUF) break;
      memcpy(priv->buf + pos, fstype, fl); pos += fl;
      memcpy(priv->buf + pos, " on ", 4); pos += 4;
      memcpy(priv->buf + pos, mpath, pl); pos += pl;
      memcpy(priv->buf + pos, " (", 2); pos += 2;
      memcpy(priv->buf + pos, rw, rl); pos += rl;
      memcpy(priv->buf + pos, ")\n", 2); pos += 2;
    }
    priv->len = pos;
    *private = priv;
    return 0;
  }
  if (strcmp(p, "cpuinfo") == 0) {
    priv->type = PROCFS_CPUINFO;
    priv->len = procfs_render_cpuinfo(priv->buf, PROCFS_BUF);
    *private = priv;
    return 0;
  }
  if (strcmp(p, "version") == 0) {
    priv->type = PROCFS_VERSION;
    priv->len = procfs_render_version(priv->buf, PROCFS_BUF);
    *private = priv;
    return 0;
  }
  if (strcmp(p, "uptime") == 0) {
    priv->type = PROCFS_UPTIME;
    priv->len = procfs_render_uptime(priv->buf, PROCFS_BUF);
    *private = priv;
    return 0;
  }
  if (strcmp(p, "meminfo") == 0) {
    priv->type = PROCFS_MEMINFO;
    priv->len = procfs_render_meminfo(priv->buf, PROCFS_BUF);
    *private = priv;
    return 0;
  }

  /* /proc/self — resolve to current process's PID */
  pid_t self_pid = current_proc ? current_proc->pid : 1;
  if (strcmp(p, "self") == 0) {
    priv->type = PROCFS_PIDDIR;
    priv->pid = self_pid;
    *private = priv;
    return 0;
  }
  /* /proc/self/<file> */
  if (strncmp(p, "self/", 5) == 0) {
    /* Rewrite as /proc/<selfpid>/<rest> by substituting self_pid */
    const char *rest = p + 5; /* after "self/" */
    /* Continue processing as if we had "<pid>/<rest>" */
    struct proc *target = find_proc(self_pid);
    if (!target) { kmfree(priv, sizeof(*priv)); return -2; }
    priv->pid = self_pid;
    if (*rest == '\0') {
      priv->type = PROCFS_PIDDIR;
    } else if (strcmp(rest, "status") == 0) {
      priv->type = PROCFS_STATUS;
      priv->len = procfs_render_status(priv->buf, PROCFS_BUF, target);
    } else if (strcmp(rest, "cmdline") == 0) {
      priv->type = PROCFS_CMDLINE;
      int nl = strlen(target->name);
      if (nl >= PROCFS_BUF) nl = PROCFS_BUF - 1;
      memcpy(priv->buf, target->name, nl);
      priv->buf[nl] = '\0';
      priv->len = nl;
    } else if (strcmp(rest, "maps") == 0) {
      priv->type = PROCFS_MAPS;
      priv->len = procfs_render_maps(priv->buf, PROCFS_BUF, target);
    } else if (strcmp(rest, "stat") == 0) {
      priv->type = PROCFS_STAT_FILE;
      priv->len = procfs_render_stat_file(priv->buf, PROCFS_BUF, target);
    } else if (strcmp(rest, "fd") == 0) {
      priv->type = PROCFS_FDDIR;
    } else if (strcmp(rest, "exe") == 0 || strcmp(rest, "cwd") == 0) {
      /* exe and cwd are symlinks; open as empty file (readlink handles them) */
      priv->type = PROCFS_PIDDIR; /* placeholder; reads return 0 */
      priv->len = 0;
    } else {
      kmfree(priv, sizeof(*priv));
      return -2;
    }
    *private = priv;
    return 0;
  }

  /* Extract PID component */
  char pidstr[16];
  int i = 0;
  while (p[i] && p[i] != '/' && i < 15) { pidstr[i] = p[i]; i++; }
  pidstr[i] = '\0';

  int pid = parse_pid(pidstr);
  if (pid < 0) { kmfree(priv, sizeof(*priv)); return -2; }

  struct proc *target = find_proc((pid_t)pid);
  if (!target) { kmfree(priv, sizeof(*priv)); return -2; }

  priv->pid = (pid_t)pid;

  /* Advance past PID */
  const char *rest = p + i;
  while (*rest == '/') rest++;

  if (*rest == '\0') {
    priv->type = PROCFS_PIDDIR;
    *private = priv;
    return 0;
  }

  if (strcmp(rest, "status") == 0) {
    priv->type = PROCFS_STATUS;
    priv->len = procfs_render_status(priv->buf, PROCFS_BUF, target);
    *private = priv;
    return 0;
  }

  if (strcmp(rest, "cmdline") == 0) {
    priv->type = PROCFS_CMDLINE;
    int nl = strlen(target->name);
    if (nl >= PROCFS_BUF) nl = PROCFS_BUF - 1;
    memcpy(priv->buf, target->name, nl);
    priv->buf[nl] = '\0';
    priv->len = nl;
    *private = priv;
    return 0;
  }

  if (strcmp(rest, "maps") == 0) {
    priv->type = PROCFS_MAPS;
    priv->len = procfs_render_maps(priv->buf, PROCFS_BUF, target);
    *private = priv;
    return 0;
  }

  if (strcmp(rest, "stat") == 0) {
    priv->type = PROCFS_STAT_FILE;
    priv->len = procfs_render_stat_file(priv->buf, PROCFS_BUF, target);
    *private = priv;
    return 0;
  }

  if (strcmp(rest, "fd") == 0) {
    priv->type = PROCFS_FDDIR;
    priv->pid = (pid_t)pid;
    *private = priv;
    return 0;
  }

  if (strcmp(rest, "exe") == 0 || strcmp(rest, "cwd") == 0) {
    /* exe and cwd are symlinks; open as placeholder (readlink handles them) */
    priv->type = PROCFS_PIDDIR; /* placeholder; reads return 0 */
    priv->len = 0;
    *private = priv;
    return 0;
  }

  kmfree(priv, sizeof(*priv));
  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* procfs_read                                                         */
/* ------------------------------------------------------------------ */
static int64_t procfs_read(void *fs_data, void *private, char *buf,
                           uint64_t count, int64_t *offset) {
  (void)fs_data;
  struct procfs_private *priv = (struct procfs_private *)private;
  if (!priv) return 0;

  if (priv->type == PROCFS_DMESG) {
    int off = (int)*offset;
    int n = dmesg_read(buf, (int)count, off);
    *offset += (int64_t)n;
    return (int64_t)n;
  }

  /* Dynamic content types: regenerate on each read at offset 0 */
  if (priv->type == PROCFS_UPTIME && *offset == 0) {
    priv->len = procfs_render_uptime(priv->buf, PROCFS_BUF);
  }
  if (priv->type == PROCFS_MEMINFO && *offset == 0) {
    priv->len = procfs_render_meminfo(priv->buf, PROCFS_BUF);
  }

  /* All other buffer-backed types */
  if (priv->type == PROCFS_ROOT || priv->type == PROCFS_PIDDIR ||
      priv->type == PROCFS_FDDIR)
    return 0; /* directories return nothing on read */

  int64_t off = *offset;
  if (off >= priv->len) return 0;

  int64_t avail = priv->len - off;
  if ((int64_t)count > avail) count = (uint64_t)avail;

  memcpy(buf, priv->buf + off, count);
  *offset += (int64_t)count;
  return (int64_t)count;
}

/* ------------------------------------------------------------------ */
/* procfs_write                                                        */
/* ------------------------------------------------------------------ */
static int64_t procfs_write(void *fs_data, void *private, const char *buf,
                            uint64_t count, int64_t *offset) {
  (void)fs_data;
  (void)private; (void)buf; (void)count; (void)offset;
  return -30; /* -EROFS */
}

/* ------------------------------------------------------------------ */
/* procfs_close                                                        */
/* ------------------------------------------------------------------ */
static int procfs_close(void *fs_data, void *private) {
  (void)fs_data;
  if (private)
    kmfree(private, sizeof(struct procfs_private));
  return 0;
}

/* ------------------------------------------------------------------ */
/* procfs_stat                                                         */
/* ------------------------------------------------------------------ */
static int procfs_stat(void *fs_data, const char *path, uint8_t *st) {
  (void)fs_data;
  const char *p = path;
  while (*p == '/') p++;

  memset(st, 0, 128);
  uint64_t dev = 4; /* procfs device */
  uint32_t nlink = 1;
  int32_t blksz = 4096;

  memcpy(st + 0, &dev, 8);
  memcpy(st + 20, &nlink, 4);
  memcpy(st + 56, &blksz, 4);

  /* Helper macros */
  #define STAT_DIR(ino_val) do { \
    uint32_t mode = (uint32_t)(0x4000 | 0555); \
    uint64_t ino = (ino_val); \
    memcpy(st + 8, &ino, 8); \
    memcpy(st + 16, &mode, 4); \
    return 0; \
  } while(0)

  #define STAT_FILE(ino_val) do { \
    uint32_t mode = (uint32_t)(0x8000 | 0444); \
    uint64_t ino = (ino_val); \
    memcpy(st + 8, &ino, 8); \
    memcpy(st + 16, &mode, 4); \
    return 0; \
  } while(0)

  #define STAT_LNK(ino_val) do { \
    uint32_t mode = (uint32_t)(0xA000 | 0777); \
    uint64_t ino = (ino_val); \
    memcpy(st + 8, &ino, 8); \
    memcpy(st + 16, &mode, 4); \
    return 0; \
  } while(0)

  if (*p == '\0') { STAT_DIR(200); }
  if (strcmp(p, "dmesg") == 0)    { STAT_FILE(201); }
  if (strcmp(p, "mounts") == 0)   { STAT_FILE(202); }
  if (strcmp(p, "cpuinfo") == 0)  { STAT_FILE(203); }
  if (strcmp(p, "version") == 0)  { STAT_FILE(204); }
  if (strcmp(p, "uptime") == 0)   { STAT_FILE(205); }
  if (strcmp(p, "meminfo") == 0)  { STAT_FILE(206); }

  /* /proc/self — symlink to current PID */
  pid_t self_pid = current_proc ? current_proc->pid : 1;
  if (strcmp(p, "self") == 0) { STAT_LNK(207); }
  if (strncmp(p, "self/", 5) == 0) {
    /* Delegate to numeric pid path */
    p += 5;
    /* Process below */
    struct proc *target = find_proc(self_pid);
    if (!target) return -2;
    if (*p == '\0') { STAT_DIR((uint64_t)(300 + self_pid)); }
    if (strcmp(p, "status") == 0 || strcmp(p, "cmdline") == 0 ||
        strcmp(p, "maps") == 0) {
      STAT_FILE((uint64_t)(1000 + self_pid * 10 + (p[0] == 's' ? (p[1]=='t' ? 1 : 3) : (p[0]=='c' ? 2 : 4))));
    }
    if (strcmp(p, "stat") == 0) {
      STAT_FILE((uint64_t)(1000 + self_pid * 10 + 5));
    }
    if (strcmp(p, "exe") == 0) {
      STAT_LNK((uint64_t)(2000 + self_pid * 10 + 1));
    }
    if (strcmp(p, "cwd") == 0) {
      STAT_LNK((uint64_t)(2000 + self_pid * 10 + 2));
    }
    if (strcmp(p, "fd") == 0) { STAT_DIR((uint64_t)(400 + self_pid)); }
    return -2;
  }

  /* Extract PID */
  char pidstr[16];
  int i = 0;
  while (p[i] && p[i] != '/' && i < 15) { pidstr[i] = p[i]; i++; }
  pidstr[i] = '\0';

  int pid = parse_pid(pidstr);
  if (pid < 0) return -2;

  struct proc *target = find_proc((pid_t)pid);
  if (!target) return -2;

  const char *rest = p + i;
  while (*rest == '/') rest++;

  if (*rest == '\0') { STAT_DIR((uint64_t)(300 + pid)); }

  if (strcmp(rest, "fd") == 0) { STAT_DIR((uint64_t)(400 + pid)); }

  if (strcmp(rest, "status") == 0 || strcmp(rest, "cmdline") == 0 ||
      strcmp(rest, "maps") == 0) {
    int sub = (rest[0] == 's') ? (rest[1] == 't' ? 1 : 3) :
              (rest[0] == 'c' ? 2 : 4);
    STAT_FILE((uint64_t)(1000 + pid * 10 + sub));
  }

  if (strcmp(rest, "stat") == 0) {
    STAT_FILE((uint64_t)(1000 + pid * 10 + 5));
  }

  if (strcmp(rest, "exe") == 0) {
    STAT_LNK((uint64_t)(2000 + pid * 10 + 1));
  }

  if (strcmp(rest, "cwd") == 0) {
    STAT_LNK((uint64_t)(2000 + pid * 10 + 2));
  }

  #undef STAT_DIR
  #undef STAT_FILE
  #undef STAT_LNK

  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* procfs_fstat                                                        */
/* ------------------------------------------------------------------ */
static int procfs_fstat(void *fs_data, void *private, uint8_t *st) {
  (void)fs_data;
  struct procfs_private *priv = (struct procfs_private *)private;
  memset(st, 0, 128);
  uint64_t dev = 4;
  uint32_t nlink = 1;
  int32_t blksz = 4096;
  memcpy(st + 0, &dev, 8);
  memcpy(st + 20, &nlink, 4);
  memcpy(st + 56, &blksz, 4);

  uint32_t dir_mode = (uint32_t)(0x4000 | 0555);
  uint32_t reg_mode = (uint32_t)(0x8000 | 0444);

  switch (priv->type) {
  case PROCFS_ROOT:
    { uint64_t ino = 200; memcpy(st+8,&ino,8); memcpy(st+16,&dir_mode,4); break; }
  case PROCFS_DMESG:
    { uint64_t ino = 201; memcpy(st+8,&ino,8); memcpy(st+16,&reg_mode,4); break; }
  case PROCFS_MOUNTS:
    { uint64_t ino = 202; uint64_t sz=(uint64_t)priv->len; memcpy(st+8,&ino,8); memcpy(st+16,&reg_mode,4); memcpy(st+48,&sz,8); break; }
  case PROCFS_CPUINFO:
    { uint64_t ino = 203; uint64_t sz=(uint64_t)priv->len; memcpy(st+8,&ino,8); memcpy(st+16,&reg_mode,4); memcpy(st+48,&sz,8); break; }
  case PROCFS_VERSION:
    { uint64_t ino = 204; uint64_t sz=(uint64_t)priv->len; memcpy(st+8,&ino,8); memcpy(st+16,&reg_mode,4); memcpy(st+48,&sz,8); break; }
  case PROCFS_UPTIME:
    { uint64_t ino = 205; uint64_t sz=(uint64_t)priv->len; memcpy(st+8,&ino,8); memcpy(st+16,&reg_mode,4); memcpy(st+48,&sz,8); break; }
  case PROCFS_MEMINFO:
    { uint64_t ino = 206; uint64_t sz=(uint64_t)priv->len; memcpy(st+8,&ino,8); memcpy(st+16,&reg_mode,4); memcpy(st+48,&sz,8); break; }
  case PROCFS_PIDDIR:
    { uint64_t ino=(uint64_t)(300+priv->pid); memcpy(st+8,&ino,8); memcpy(st+16,&dir_mode,4); break; }
  case PROCFS_FDDIR:
    { uint64_t ino=(uint64_t)(400+priv->pid); memcpy(st+8,&ino,8); memcpy(st+16,&dir_mode,4); break; }
  default: /* STATUS, CMDLINE, MAPS */
    { uint64_t ino=(uint64_t)(1000+priv->pid*10+(priv->type==PROCFS_STATUS?1:priv->type==PROCFS_CMDLINE?2:3));
      uint64_t sz=(uint64_t)priv->len;
      memcpy(st+8,&ino,8); memcpy(st+16,&reg_mode,4); memcpy(st+48,&sz,8); break; }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* procfs_readdir                                                      */
/* ------------------------------------------------------------------ */
static int procfs_readdir(void *fs_data, void *private, char *ubuf,
                          unsigned int count, int64_t *offset) {
  (void)fs_data;
  struct procfs_private *priv = (struct procfs_private *)private;
  if (!priv) return -1;

  uint64_t skip = (uint64_t)*offset;
  unsigned int written = 0;
  uint64_t seen = 0;

  #define EMIT_DIRENT(name_str, ino_val, dtype) do { \
    if (seen >= skip) { \
      int nl = strlen(name_str); \
      int rl = (19 + nl + 1 + 7) & ~7; \
      if (written + (unsigned int)rl > count) goto done; \
      char *d = ubuf + written; \
      memset(d, 0, rl); \
      uint64_t _ino = (ino_val); \
      int64_t _doff = (int64_t)(seen + 1); \
      uint16_t _rlen = (uint16_t)rl; \
      uint8_t _dt = (dtype); \
      memcpy(d + 0, &_ino, 8); \
      memcpy(d + 8, &_doff, 8); \
      memcpy(d + 16, &_rlen, 2); \
      memcpy(d + 18, &_dt, 1); \
      memcpy(d + 19, name_str, nl + 1); \
      written += (unsigned int)rl; \
      *offset = (int64_t)(seen + 1); \
    } \
    seen++; \
  } while(0)

  if (priv->type == PROCFS_ROOT) {
    EMIT_DIRENT(".", 200, 4);
    EMIT_DIRENT("..", 1, 4);
    EMIT_DIRENT("dmesg",   201, 8);
    EMIT_DIRENT("mounts",  202, 8);
    EMIT_DIRENT("cpuinfo", 203, 8);
    EMIT_DIRENT("version", 204, 8);
    EMIT_DIRENT("uptime",  205, 8);
    EMIT_DIRENT("meminfo", 206, 8);
    /* /proc/self as a symlink entry */
    EMIT_DIRENT("self", 207, 10); /* DT_LNK */

    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
      if (p->state != UNUSED) {
        char pidname[12];
        int ni = 0;
        int v = p->pid;
        if (v == 0) { pidname[ni++] = '0'; }
        else { char tmp[12]; int ti = 0; while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; } while (ti > 0) pidname[ni++] = tmp[--ti]; }
        pidname[ni] = '\0';
        EMIT_DIRENT(pidname, (uint64_t)(300 + p->pid), 4);
      }
    }
  } else if (priv->type == PROCFS_PIDDIR) {
    uint64_t base = (uint64_t)(300 + priv->pid);
    EMIT_DIRENT(".", base, 4);
    EMIT_DIRENT("..", 200, 4);
    EMIT_DIRENT("status",  (uint64_t)(1000 + priv->pid * 10 + 1), 8);
    EMIT_DIRENT("cmdline", (uint64_t)(1000 + priv->pid * 10 + 2), 8);
    EMIT_DIRENT("maps",    (uint64_t)(1000 + priv->pid * 10 + 3), 8);
    EMIT_DIRENT("stat",    (uint64_t)(1000 + priv->pid * 10 + 5), 8);
    EMIT_DIRENT("fd",      (uint64_t)(400 + priv->pid), 4);
    EMIT_DIRENT("exe",     (uint64_t)(2000 + priv->pid * 10 + 1), 10); /* DT_LNK */
    EMIT_DIRENT("cwd",     (uint64_t)(2000 + priv->pid * 10 + 2), 10); /* DT_LNK */
  } else if (priv->type == PROCFS_FDDIR) {
    /* List open file descriptors */
    uint64_t base = (uint64_t)(400 + priv->pid);
    EMIT_DIRENT(".", base, 4);
    EMIT_DIRENT("..", (uint64_t)(300 + priv->pid), 4);
    struct proc *p = find_proc(priv->pid);
    if (p) {
      struct open_file **ftable = proc_ofile(p);
      for (int fd = 0; fd < NOFILE; fd++) {
        if (ftable[fd]) {
          /* fd number as string */
          char fdname[8];
          int ni = 0;
          int v = fd;
          if (v == 0) { fdname[ni++] = '0'; }
          else { char tmp[8]; int ti = 0; while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; } while (ti > 0) fdname[ni++] = tmp[--ti]; }
          fdname[ni] = '\0';
          EMIT_DIRENT(fdname, (uint64_t)(5000 + priv->pid * 16 + fd), 10); /* DT_LNK */
        }
      }
    }
  }

  #undef EMIT_DIRENT

done:
  return (int)written;
}

/* ------------------------------------------------------------------ */
/* procfs stubs                                                        */
/* ------------------------------------------------------------------ */
static int procfs_mkdir(void *fs_data, const char *path, int mode) {
  (void)fs_data;
  (void)path; (void)mode;
  return -30; /* -EROFS */
}

static int procfs_unlink(void *fs_data, const char *path) {
  (void)fs_data;
  (void)path;
  return -30; /* -EROFS */
}

/* ------------------------------------------------------------------ */
/* procfs_readlink                                                     */
/* ------------------------------------------------------------------ */
static int procfs_readlink(void *fs_data, const char *path, char *buf,
                           int bufsiz) {
  (void)fs_data;
  const char *p = path;
  while (*p == '/') p++;

  /* /proc/self -> "<pid>" */
  if (strcmp(p, "self") == 0) {
    pid_t self_pid = current_proc ? current_proc->pid : 1;
    char tmp[16];
    int ti = 0;
    int v = (int)self_pid;
    if (v == 0) { tmp[ti++] = '0'; }
    else { char rev[16]; int ri = 0; while (v > 0) { rev[ri++] = '0' + (v % 10); v /= 10; } while (ri > 0) tmp[ti++] = rev[--ri]; }
    tmp[ti] = '\0';
    int len = ti;
    if (len > bufsiz) len = bufsiz;
    memcpy(buf, tmp, len);
    return len;
  }

  /* Determine target PID and sub-path */
  pid_t target_pid;
  const char *rest;

  if (strncmp(p, "self/", 5) == 0) {
    target_pid = current_proc ? current_proc->pid : 1;
    rest = p + 5;
  } else {
    /* Extract PID component */
    char pidstr[16];
    int i = 0;
    while (p[i] && p[i] != '/' && i < 15) { pidstr[i] = p[i]; i++; }
    pidstr[i] = '\0';
    int pid = parse_pid(pidstr);
    if (pid < 0) return -22; /* EINVAL */
    target_pid = (pid_t)pid;
    rest = p + i;
    while (*rest == '/') rest++;
  }

  struct proc *target = find_proc(target_pid);
  if (!target) return -2; /* ENOENT */

  /* /proc/<pid>/exe -> exe_path */
  if (strcmp(rest, "exe") == 0) {
    int len = strlen(target->exe_path);
    if (len == 0) return -2; /* no exe path set */
    if (len > bufsiz) len = bufsiz;
    memcpy(buf, target->exe_path, len);
    return len;
  }

  /* /proc/<pid>/cwd -> cwd */
  if (strcmp(rest, "cwd") == 0) {
    int len = strlen(target->cwd);
    if (len == 0) { buf[0] = '/'; return 1; }
    if (len > bufsiz) len = bufsiz;
    memcpy(buf, target->cwd, len);
    return len;
  }

  return -22; /* EINVAL — not a symlink */
}

/* ------------------------------------------------------------------ */
/* procfs VFS ops struct                                               */
/* ------------------------------------------------------------------ */
struct vfs_ops procfs_ops = {
  .open     = procfs_open,
  .read     = procfs_read,
  .write    = procfs_write,
  .close    = procfs_close,
  .stat     = procfs_stat,
  .fstat    = procfs_fstat,
  .readdir  = procfs_readdir,
  .mkdir    = procfs_mkdir,
  .unlink   = procfs_unlink,
  .rmdir    = NULL,
  .readlink = procfs_readlink,
};
