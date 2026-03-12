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
  PROCFS_STAT,      /* /proc/<pid>/stat (single-line) */
  PROCFS_CMDLINE,   /* /proc/<pid>/cmdline */
  PROCFS_FDDIR,     /* /proc/<pid>/fd directory */
  PROCFS_FDLINK,    /* /proc/<pid>/fd/<n> pseudo-symlink */
  PROCFS_MAPS,      /* /proc/<pid>/maps */
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
  for (struct proc *p = proc; p < &proc[64]; p++) {
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
  pos = append_int(buf, bufsz, pos, (long)p->sigpending);
  if (pos < bufsz) buf[pos++] = '\n';

  pos = append_str(buf, bufsz, pos, "SigBlk:\t");
  pos = append_int(buf, bufsz, pos, (long)p->sigmask);
  if (pos < bufsz) buf[pos++] = '\n';

  return pos;
}

/* Render /proc/<pid>/stat (single-line Linux format) */
static int procfs_render_stat(char *buf, int bufsz, struct proc *p) {
  int pos = 0;
  pid_t ppid = p->parent ? p->parent->pid : 0;
  char stch;
  switch (p->state) {
    case SLEEPING: stch = 'S'; break;
    case RUNNABLE: case RUNNING: stch = 'R'; break;
    case ZOMBIE:   stch = 'Z'; break;
    default:       stch = '?'; break;
  }
  /* pid (comm) state ppid pgrp session tty_nr ... */
  pos = append_int(buf, bufsz, pos, p->pid);
  pos = append_str(buf, bufsz, pos, " (");
  pos = append_str(buf, bufsz, pos, p->name);
  pos = append_str(buf, bufsz, pos, ") ");
  if (pos < bufsz) buf[pos++] = stch;
  if (pos < bufsz) buf[pos++] = ' ';
  pos = append_int(buf, bufsz, pos, ppid);
  if (pos < bufsz) buf[pos++] = ' ';
  pos = append_int(buf, bufsz, pos, p->pgid);
  if (pos < bufsz) buf[pos++] = ' ';
  pos = append_int(buf, bufsz, pos, p->sid);
  /* tty_nr=0, tpgid=0, flags=0, and remaining fields as zeros */
  pos = append_str(buf, bufsz, pos, " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
  pos = append_int(buf, bufsz, pos, (long)(p->sz / 4096)); /* rss (in pages) */
  pos = append_str(buf, bufsz, pos, " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
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
static int procfs_open(const char *path, int flags, void **private) {
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
    } else if (strcmp(rest, "stat") == 0) {
      priv->type = PROCFS_STAT;
      priv->len = procfs_render_stat(priv->buf, PROCFS_BUF, target);
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
    } else if (strcmp(rest, "fd") == 0) {
      priv->type = PROCFS_FDDIR;
    } else if (strncmp(rest, "fd/", 3) == 0) {
      /* /proc/self/fd/<n> — pseudo-symlink to open file path */
      int fdn = parse_pid(rest + 3);
      if (fdn < 0 || fdn >= NOFILE) { kmfree(priv, sizeof(*priv)); return -2; }
      struct open_file **ftable = proc_ofile(target);
      if (!ftable[fdn]) { kmfree(priv, sizeof(*priv)); return -2; }
      priv->type = PROCFS_FDLINK;
      int pl = strlen(ftable[fdn]->path);
      if (pl >= PROCFS_BUF) pl = PROCFS_BUF - 1;
      memcpy(priv->buf, ftable[fdn]->path, pl);
      priv->buf[pl] = '\0';
      priv->len = pl;
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

  if (strcmp(rest, "stat") == 0) {
    priv->type = PROCFS_STAT;
    priv->len = procfs_render_stat(priv->buf, PROCFS_BUF, target);
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

  if (strcmp(rest, "fd") == 0) {
    priv->type = PROCFS_FDDIR;
    priv->pid = (pid_t)pid;
    *private = priv;
    return 0;
  }

  if (strncmp(rest, "fd/", 3) == 0) {
    int fdn = parse_pid(rest + 3);
    if (fdn < 0 || fdn >= NOFILE) { kmfree(priv, sizeof(*priv)); return -2; }
    struct open_file **ftable = proc_ofile(target);
    if (!ftable[fdn]) { kmfree(priv, sizeof(*priv)); return -2; }
    priv->type = PROCFS_FDLINK;
    priv->pid = (pid_t)pid;
    int pl = strlen(ftable[fdn]->path);
    if (pl >= PROCFS_BUF) pl = PROCFS_BUF - 1;
    memcpy(priv->buf, ftable[fdn]->path, pl);
    priv->buf[pl] = '\0';
    priv->len = pl;
    *private = priv;
    return 0;
  }

  kmfree(priv, sizeof(*priv));
  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* procfs_read                                                         */
/* ------------------------------------------------------------------ */
static int64_t procfs_read(void *private, char *buf, uint64_t count, int64_t *offset) {
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
static int64_t procfs_write(void *private, const char *buf, uint64_t count, int64_t *offset) {
  (void)private; (void)buf; (void)count; (void)offset;
  return -30; /* -EROFS */
}

/* ------------------------------------------------------------------ */
/* procfs_close                                                        */
/* ------------------------------------------------------------------ */
static int procfs_close(void *private) {
  if (private)
    kmfree(private, sizeof(struct procfs_private));
  return 0;
}

/* ------------------------------------------------------------------ */
/* procfs_stat                                                         */
/* ------------------------------------------------------------------ */
static int procfs_stat(const char *path, uint8_t *st) {
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

  if (*p == '\0') { STAT_DIR(200); }
  if (strcmp(p, "dmesg") == 0)    { STAT_FILE(201); }
  if (strcmp(p, "mounts") == 0)   { STAT_FILE(202); }
  if (strcmp(p, "cpuinfo") == 0)  { STAT_FILE(203); }
  if (strcmp(p, "version") == 0)  { STAT_FILE(204); }
  if (strcmp(p, "uptime") == 0)   { STAT_FILE(205); }
  if (strcmp(p, "meminfo") == 0)  { STAT_FILE(206); }

  /* /proc/self — treat as a directory for stat purposes */
  pid_t self_pid = current_proc ? current_proc->pid : 1;
  if (strcmp(p, "self") == 0) { STAT_DIR((uint64_t)(300 + self_pid)); }
  if (strncmp(p, "self/", 5) == 0) {
    /* Delegate to numeric pid path */
    p += 5;
    /* Process below */
    struct proc *target = find_proc(self_pid);
    if (!target) return -2;
    if (*p == '\0') { STAT_DIR((uint64_t)(300 + self_pid)); }
    if (strcmp(p, "status") == 0 || strcmp(p, "stat") == 0 ||
        strcmp(p, "cmdline") == 0 || strcmp(p, "maps") == 0) {
      STAT_FILE((uint64_t)(1000 + self_pid * 10 + 1));
    }
    if (strcmp(p, "fd") == 0) { STAT_DIR((uint64_t)(400 + self_pid)); }
    if (strncmp(p, "fd/", 3) == 0) {
      /* stat on /proc/self/fd/<n> — treat as symlink */
      uint32_t lnk_mode = (uint32_t)(0xA000 | 0777);
      uint64_t ino = (uint64_t)(5000 + self_pid * 16);
      memcpy(st + 8, &ino, 8);
      memcpy(st + 16, &lnk_mode, 4);
      return 0;
    }
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

  if (strcmp(rest, "status") == 0 || strcmp(rest, "stat") == 0 ||
      strcmp(rest, "cmdline") == 0 || strcmp(rest, "maps") == 0) {
    STAT_FILE((uint64_t)(1000 + pid * 10 + 1));
  }

  if (strncmp(rest, "fd/", 3) == 0) {
    uint32_t lnk_mode = (uint32_t)(0xA000 | 0777);
    uint64_t ino = (uint64_t)(5000 + pid * 16);
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &lnk_mode, 4);
    return 0;
  }

  #undef STAT_DIR
  #undef STAT_FILE

  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* procfs_fstat                                                        */
/* ------------------------------------------------------------------ */
static int procfs_fstat(void *private, uint8_t *st) {
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
static int procfs_readdir(void *private, char *ubuf, unsigned int count, int64_t *offset) {
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
    /* /proc/self as a directory entry */
    EMIT_DIRENT("self", 207, 4);

    for (struct proc *p = proc; p < &proc[64]; p++) {
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
    EMIT_DIRENT("stat",    (uint64_t)(1000 + priv->pid * 10 + 5), 8);
    EMIT_DIRENT("cmdline", (uint64_t)(1000 + priv->pid * 10 + 2), 8);
    EMIT_DIRENT("maps",    (uint64_t)(1000 + priv->pid * 10 + 3), 8);
    EMIT_DIRENT("fd",      (uint64_t)(400 + priv->pid), 4);
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
static int procfs_mkdir(const char *path, int mode) {
  (void)path; (void)mode;
  return -30; /* -EROFS */
}

static int procfs_unlink(const char *path) {
  (void)path;
  return -30; /* -EROFS */
}

/* ------------------------------------------------------------------ */
/* procfs_readlink                                                     */
/* ------------------------------------------------------------------ */
static int procfs_readlink(const char *path, char *buf, int bufsiz) {
  const char *p = path;
  while (*p == '/') p++;

  /* /proc/self → "<pid>" */
  pid_t self_pid = current_proc ? current_proc->pid : 1;
  if (strcmp(p, "self") == 0) {
    char tmp[12];
    int ti = 0;
    int v = self_pid;
    if (v == 0) { tmp[ti++] = '0'; }
    else { char r[12]; int ri = 0; while (v > 0) { r[ri++] = '0' + (v % 10); v /= 10; } while (ri > 0) tmp[ti++] = r[--ri]; }
    int n = ti < bufsiz ? ti : bufsiz;
    memcpy(buf, tmp, n);
    return n;
  }

  /* /proc/<pid>/fd/<n> or /proc/self/fd/<n> */
  pid_t target_pid = -1;
  const char *rest = 0;
  if (strncmp(p, "self/", 5) == 0) {
    target_pid = self_pid;
    rest = p + 5;
  } else {
    char pidstr[16];
    int i = 0;
    while (p[i] && p[i] != '/' && i < 15) { pidstr[i] = p[i]; i++; }
    pidstr[i] = '\0';
    target_pid = (pid_t)parse_pid(pidstr);
    rest = p + i;
    while (*rest == '/') rest++;
  }
  if (target_pid >= 0 && strncmp(rest, "fd/", 3) == 0) {
    int fdn = parse_pid(rest + 3);
    if (fdn < 0 || fdn >= NOFILE) return -22;
    struct proc *target = find_proc(target_pid);
    if (!target) return -2;
    struct open_file **ftable = proc_ofile(target);
    if (!ftable[fdn]) return -2;
    int pl = strlen(ftable[fdn]->path);
    int n = pl < bufsiz ? pl : bufsiz;
    memcpy(buf, ftable[fdn]->path, n);
    return n;
  }
  return -22; /* -EINVAL: not a symlink */
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
