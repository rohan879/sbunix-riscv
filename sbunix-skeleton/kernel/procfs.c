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
#include <errno.h>

/* Types of procfs entries */
enum procfs_type {
  PROCFS_ROOT,      /* /proc directory */
  PROCFS_PIDDIR,    /* /proc/<pid> directory */
  PROCFS_STATUS,    /* /proc/<pid>/status */
  PROCFS_CMDLINE,   /* /proc/<pid>/cmdline */
  PROCFS_FDDIR,     /* /proc/<pid>/fd directory */
  PROCFS_FDLINK,    /* /proc/<pid>/fd/<n> symlink */
  PROCFS_MAPS,      /* /proc/<pid>/maps */
  PROCFS_STAT_FILE, /* /proc/<pid>/stat (single-line format) */
  PROCFS_EXE_LINK,  /* /proc/<pid>/exe symlink */
  PROCFS_CWD_LINK,  /* /proc/<pid>/cwd symlink */
  PROCFS_DMESG,     /* /proc/dmesg */
  PROCFS_MOUNTS,    /* /proc/mounts */
  PROCFS_CPUINFO,   /* /proc/cpuinfo */
  PROCFS_VERSION,   /* /proc/version */
  PROCFS_UPTIME,    /* /proc/uptime */
  PROCFS_MEMINFO,   /* /proc/meminfo */
};

/* Buffer size for rendered content */
#define PROCFS_BUF 1024
#define PROCFS_O_ACCMODE 3
#define PROCFS_O_WRONLY  1
#define PROCFS_O_RDWR    2
#define PROCFS_O_CREAT   0100
#define PROCFS_O_TRUNC   01000
#define PROCFS_O_NOFOLLOW 0400000

struct procfs_private {
  enum procfs_type type;
  pid_t pid;
  int fd;
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
  if (pos >= bufsz)
    return pos;
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

static int append_hex_min(char *buf, int bufsz, int pos, uint64_t v) {
  char tmp[17];
  int ti = 0;
  int seen = 0;

  for (int shift = 60; shift >= 0; shift -= 4) {
    int nibble = (int)((v >> shift) & 0xF);
    if (nibble || seen || shift == 0) {
      tmp[ti++] = "0123456789abcdef"[nibble];
      seen = 1;
    }
  }
  tmp[ti] = '\0';
  return append_str(buf, bufsz, pos, tmp);
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

static int parse_procfs_fd_leaf(const char *rest) {
  if (strncmp(rest, "fd/", 3) != 0)
    return -1;
  rest += 3;
  if (*rest == '\0')
    return -1;
  return parse_pid(rest);
}

static int procfs_check_read_open_flags(int flags) {
  int accmode = flags & PROCFS_O_ACCMODE;

  /* BF: procfs has no writable backing store; fail mutating opens before a
   * pseudo-file private object is allocated or reopened. */
  if (flags & (PROCFS_O_CREAT | PROCFS_O_TRUNC))
    return -EROFS;
  if (accmode == PROCFS_O_WRONLY || accmode == PROCFS_O_RDWR)
    return -EACCES;
  return 0;
}

/* Helper: find a process by PID */
static struct proc *find_proc(pid_t pid) {
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->pid == pid && p->state != UNUSED)
      return p;
  }
  return 0;
}

static struct proc *procfs_find_target(pid_t pid) {
  if (current_proc && current_proc->pid == pid && current_proc->state != UNUSED)
    return current_proc;
  return find_proc(pid);
}

static int procfs_is_dir(enum procfs_type type) {
  return type == PROCFS_ROOT || type == PROCFS_PIDDIR || type == PROCFS_FDDIR;
}

static int procfs_is_link(enum procfs_type type) {
  return type == PROCFS_FDLINK || type == PROCFS_EXE_LINK ||
         type == PROCFS_CWD_LINK;
}

static int procfs_pid_file_type(const char *name, enum procfs_type *type) {
  if (strcmp(name, "status") == 0) {
    *type = PROCFS_STATUS;
    return 0;
  }
  if (strcmp(name, "cmdline") == 0) {
    *type = PROCFS_CMDLINE;
    return 0;
  }
  if (strcmp(name, "maps") == 0) {
    *type = PROCFS_MAPS;
    return 0;
  }
  if (strcmp(name, "stat") == 0) {
    *type = PROCFS_STAT_FILE;
    return 0;
  }
  return -ENOENT;
}

static int procfs_pid_file_sub(enum procfs_type type) {
  switch (type) {
  case PROCFS_STATUS:
    return 1;
  case PROCFS_CMDLINE:
    return 2;
  case PROCFS_MAPS:
    return 3;
  case PROCFS_STAT_FILE:
    return 5;
  default:
    return 0;
  }
}

static uint64_t procfs_pid_file_ino(pid_t pid, enum procfs_type type) {
  /* BF: Keep stat(), fstat(), and getdents64() on one inode numbering scheme. */
  return (uint64_t)(1000 + pid * 10 + procfs_pid_file_sub(type));
}

static int procfs_fd_target_len(struct proc *target, int fd) {
  if (!target || fd < 0 || fd >= NOFILE)
    return 0;
  struct open_file **ftable = proc_ofile(target);
  struct open_file *f = ftable[fd];
  if (!f)
    return 0;
  if (f->path[0] != '\0')
    return strlen(f->path);

  char tmp[64];
  int len = 0;
  if (f->is_pipe) {
    len = append_str(tmp, sizeof(tmp), len, "pipe:[");
    len = append_int(tmp, sizeof(tmp), len, f->pipe_id);
  } else {
    len = append_str(tmp, sizeof(tmp), len, "anon:[");
    len = append_int(tmp, sizeof(tmp), len, fd);
  }
  if (len < (int)sizeof(tmp))
    tmp[len++] = ']';
  return len;
}

static int procfs_link_target_len(struct proc *target, enum procfs_type type,
                                  int fd) {
  if (!target)
    return 0;
  if (type == PROCFS_EXE_LINK)
    return strlen(target->exe_path);
  if (type == PROCFS_CWD_LINK)
    return target->cwd[0] ? strlen(target->cwd) : 1;
  if (type == PROCFS_FDLINK)
    return procfs_fd_target_len(target, fd);
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

static uint64_t procfs_vmsize_bytes(struct proc *p) {
  uint64_t total;
  uint64_t covered_end;

  if (!p)
    return 0;

  total = PGROUNDUP(p->sz);
  covered_end = total;

  for (struct vm_area *vma = p->vma_list; vma; vma = vma->next) {
    uint64_t start = vma->start;
    uint64_t end = vma->end;
    uint64_t add_start;
    uint64_t add;

    if (end <= start || end <= covered_end)
      continue;
    add_start = start < covered_end ? covered_end : start;
    add = end - add_start;
    if (total > (uint64_t)-1 - add)
      return (uint64_t)-1;
    total += add;
    covered_end = end;
  }

  return total;
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
  pos = append_int(buf, bufsz, pos, (long)(procfs_vmsize_bytes(p) / 1024));
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
    for (int s = 1; s < 65; s++)
      if (sa[s].handler == 1) sig_ign |= signal_bit(s); /* SIG_IGN = 1 */
    pos = append_hex16(buf, bufsz, pos, sig_ign);
  }
  if (pos < bufsz) buf[pos++] = '\n';

  pos = append_str(buf, bufsz, pos, "SigCgt:\t");
  {
    uint64_t sig_cgt = 0;
    struct kern_sigaction *sa = proc_sigactions(p);
    for (int s = 1; s < 65; s++)
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

  /* BF: Fields 7-52 (46 fields) following pid(1) (comm)(2) state(3) ppid(4)
   * pgid(5) sid(6).  The pre-vsize string must have exactly 16 values
   * (fields 7-22); the previous version had 18, shifting vsize from
   * field 23 to 25 and breaking Linux /proc/pid/stat parsers.
   * tty_nr(7) tpgid(8) flags(9) minflt(10) cminflt(11)
   * majflt(12) cmajflt(13) utime(14) stime(15) cutime(16) cstime(17)
   * priority(18)=20 nice(19)=0 num_threads(20)=1 itrealvalue(21)=0
   * starttime(22)=0  =>  vsize(23)  =>  rss(24) ... exit_code(52) */
  pos = append_str(buf, bufsz, pos,
    "0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 0 ");
  pos = append_int(buf, bufsz, pos, (long)procfs_vmsize_bytes(p)); /* vsize */
  /* BF: Fields 24-52 per Linux proc(5). exit_signal is field 38 (SIGCHLD).
   * Previously had 17 at field 37 (cnswap) instead of field 38.
   * Layout: 14 zeros (fields 24-37), SIGCHLD (field 38), 14 zeros (39-52). */
  pos = append_str(buf, bufsz, pos,
    " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
  pos = append_int(buf, bufsz, pos, SIGCHLD); /* field 38: exit_signal */
  pos = append_str(buf, bufsz, pos,
    " 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");

  return pos;
}

static int procfs_render_maps_line(char *buf, int bufsz, struct vm_area *vma) {
  int pos = 0;

  pos = append_hex_min(buf, bufsz, pos, vma->start);
  if (pos < bufsz) buf[pos++] = '-';
  pos = append_hex_min(buf, bufsz, pos, vma->end);
  if (pos < bufsz) buf[pos++] = ' ';
  if (pos < bufsz) buf[pos++] = (vma->prot & PROT_READ) ? 'r' : '-';
  if (pos < bufsz) buf[pos++] = (vma->prot & PROT_WRITE) ? 'w' : '-';
  if (pos < bufsz) buf[pos++] = (vma->prot & PROT_EXEC) ? 'x' : '-';
  if (pos < bufsz) buf[pos++] = (vma->flags & MAP_SHARED) ? 's' : 'p';
  pos = append_str(buf, bufsz, pos, " 00000000 00:00 0\n");

  return pos;
}

static uint64_t procfs_maps_size(struct proc *p) {
  uint64_t total = 0;
  char line[96];

  if (!p)
    return 0;
  for (struct vm_area *vma = p->vma_list; vma; vma = vma->next) {
    int len = procfs_render_maps_line(line, sizeof(line), vma);
    if (total > (uint64_t)-1 - (uint64_t)len)
      return (uint64_t)-1;
    total += (uint64_t)len;
  }
  return total;
}

static int64_t procfs_read_maps(struct procfs_private *priv, char *buf,
                                uint64_t count, int64_t *offset) {
  struct proc *target;
  uint64_t skip;
  uint64_t copied = 0;
  char line[96];

  if (*offset < 0)
    return -EINVAL;
  target = procfs_find_target(priv->pid);
  if (!target)
    return -ENOENT;

  skip = (uint64_t)*offset;
  for (struct vm_area *vma = target->vma_list; vma; vma = vma->next) {
    int len = procfs_render_maps_line(line, sizeof(line), vma);
    uint64_t take;

    if (skip >= (uint64_t)len) {
      skip -= (uint64_t)len;
      continue;
    }

    take = (uint64_t)len - skip;
    if (take > count - copied)
      take = count - copied;
    memcpy(buf + copied, line + skip, take);
    copied += take;
    skip = 0;
    if (copied == count)
      break;
  }

  *offset += (int64_t)copied;
  return (int64_t)copied;
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
  /* BF: Report the configured RAM size rather than the old fixed value. */
  long total_kb = (long)((PHYSTOP - KERNBASE) / 1024);
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

static int procfs_render_mounts(char *buf, int bufsz) {
  int pos = 0;
  const char *mpath, *fstype;

  for (int i = 0; vfs_get_mount_info(i, &mpath, &fstype) == 0; i++) {
    const char *rw =
      (strcmp(fstype, "ext2") == 0 || strcmp(fstype, "ramfs") == 0) ?
      "rw" : "ro";
    int before = pos;
    pos = append_str(buf, bufsz, pos, fstype);
    if (pos < bufsz) buf[pos++] = ' ';
    pos = append_str(buf, bufsz, pos, mpath);
    if (pos < bufsz) buf[pos++] = ' ';
    pos = append_str(buf, bufsz, pos, fstype);
    if (pos < bufsz) buf[pos++] = ' ';
    pos = append_str(buf, bufsz, pos, rw);
    pos = append_str(buf, bufsz, pos, " 0 0\n");
    if (pos >= bufsz) {
      pos = before;
      break;
    }
  }
  return pos;
}

static uint64_t procfs_cmdline_size(struct proc *target) {
  int n;

  if (!target)
    return 0;
  n = (int)target->cmdline_len;
  if (n <= 0) {
    int nl = strlen(target->name);
    if (nl >= PROCFS_BUF)
      nl = PROCFS_BUF - 1;
    return (uint64_t)(nl + 1);
  }
  if (n > PROCFS_BUF)
    n = PROCFS_BUF;
  return (uint64_t)n;
}

static int procfs_rendered_size(enum procfs_type type, struct proc *target,
                                uint64_t *size_out) {
  char *tmp;

  if (!size_out)
    return -EINVAL;
  *size_out = 0;

  if (type == PROCFS_CMDLINE) {
    *size_out = procfs_cmdline_size(target);
    return 0;
  }
  if (type == PROCFS_MAPS) {
    *size_out = target ? procfs_maps_size(target) : 0;
    return 0;
  }

  tmp = (char *)kmalloc(PROCFS_BUF);
  if (!tmp)
    return -ENOMEM;

  /* BF: path stat cannot reuse an open private buffer, so render into a
   * bounded heap scratch buffer using the same truncation limit as procfs_open().
   * Report allocation failure to stat() instead of claiming pseudo-files are empty. */
  switch (type) {
  case PROCFS_MOUNTS:
    *size_out = (uint64_t)procfs_render_mounts(tmp, PROCFS_BUF);
    break;
  case PROCFS_CPUINFO:
    *size_out = (uint64_t)procfs_render_cpuinfo(tmp, PROCFS_BUF);
    break;
  case PROCFS_VERSION:
    *size_out = (uint64_t)procfs_render_version(tmp, PROCFS_BUF);
    break;
  case PROCFS_UPTIME:
    *size_out = (uint64_t)procfs_render_uptime(tmp, PROCFS_BUF);
    break;
  case PROCFS_MEMINFO:
    *size_out = (uint64_t)procfs_render_meminfo(tmp, PROCFS_BUF);
    break;
  case PROCFS_STATUS:
    *size_out = target ? (uint64_t)procfs_render_status(tmp, PROCFS_BUF, target) : 0;
    break;
  case PROCFS_STAT_FILE:
    *size_out = target ? (uint64_t)procfs_render_stat_file(tmp, PROCFS_BUF, target) : 0;
    break;
  default:
    break;
  }
  kmfree(tmp, PROCFS_BUF);
  return 0;
}

/* ------------------------------------------------------------------ */
/* procfs_open                                                         */
/* ------------------------------------------------------------------ */
static int procfs_open(void *fs_data, const char *path, int flags, void **private) {
  (void)fs_data;
  const char *p = path;
  int nofollow = (flags & PROCFS_O_NOFOLLOW);
  int flag_ret;
  while (*p == '/') p++;

  flag_ret = procfs_check_read_open_flags(flags);
  if (flag_ret < 0)
    return flag_ret;

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
    priv->len = procfs_render_mounts(priv->buf, PROCFS_BUF);
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
    if (nofollow) {
      kmfree(priv, sizeof(*priv));
      return -40; /* -ELOOP */
    }
    priv->type = PROCFS_PIDDIR;
    priv->pid = self_pid;
    *private = priv;
    return 0;
  }
  /* /proc/self/<file> */
  if (strncmp(p, "self/", 5) == 0) {
    /* Rewrite as /proc/<selfpid>/<rest> by substituting self_pid */
    const char *rest = p + 5; /* after "self/" */
    /* BF: /proc/self must describe the live caller even when that short-lived
     * child has not become discoverable through a proc-table lookup yet. */
    struct proc *target = procfs_find_target(self_pid);
    if (!target) { kmfree(priv, sizeof(*priv)); return -2; }
    priv->pid = self_pid;
    if (*rest == '\0') {
      priv->type = PROCFS_PIDDIR;
    } else if (strcmp(rest, "status") == 0) {
      priv->type = PROCFS_STATUS;
      priv->len = procfs_render_status(priv->buf, PROCFS_BUF, target);
    } else if (strcmp(rest, "cmdline") == 0) {
      priv->type = PROCFS_CMDLINE;
      int n = (int)target->cmdline_len;
      if (n <= 0) {
        int nl = strlen(target->name);
        if (nl >= PROCFS_BUF) nl = PROCFS_BUF - 1;
        memcpy(priv->buf, target->name, nl);
        priv->buf[nl] = '\0';
        priv->len = nl + 1;
      } else {
        if (n > PROCFS_BUF) n = PROCFS_BUF;
        memcpy(priv->buf, target->cmdline, n);
        priv->len = n;
      }
    } else if (strcmp(rest, "maps") == 0) {
      priv->type = PROCFS_MAPS;
      priv->len = (int)procfs_maps_size(target);
    } else if (strcmp(rest, "stat") == 0) {
      priv->type = PROCFS_STAT_FILE;
      priv->len = procfs_render_stat_file(priv->buf, PROCFS_BUF, target);
    } else if (strcmp(rest, "fd") == 0) {
      priv->type = PROCFS_FDDIR;
    } else if (strcmp(rest, "exe") == 0) {
      if (nofollow) {
        kmfree(priv, sizeof(*priv));
        return -40;
      }
      priv->type = PROCFS_EXE_LINK;
      priv->len = 0;
    } else if (strcmp(rest, "cwd") == 0) {
      if (nofollow) {
        kmfree(priv, sizeof(*priv));
        return -40;
      }
      priv->type = PROCFS_CWD_LINK;
      priv->len = 0;
    } else {
      int fd = parse_procfs_fd_leaf(rest);
      struct open_file **ftable;
      if (fd < 0) {
        kmfree(priv, sizeof(*priv));
        return -2;
      }
      ftable = proc_ofile(target);
      if (fd >= NOFILE || !ftable[fd]) {
        kmfree(priv, sizeof(*priv));
        return -2;
      }
      if (nofollow) {
        kmfree(priv, sizeof(*priv));
        return -40;
      }
      priv->type = PROCFS_FDLINK;
      priv->fd = fd;
      priv->len = 0;
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

  struct proc *target = procfs_find_target((pid_t)pid);
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
    int n = (int)target->cmdline_len;
    if (n <= 0) {
      int nl = strlen(target->name);
      if (nl >= PROCFS_BUF) nl = PROCFS_BUF - 1;
      memcpy(priv->buf, target->name, nl);
      priv->buf[nl] = '\0';
      priv->len = nl + 1;
    } else {
      if (n > PROCFS_BUF) n = PROCFS_BUF;
      memcpy(priv->buf, target->cmdline, n);
      priv->len = n;
    }
    *private = priv;
    return 0;
  }

  if (strcmp(rest, "maps") == 0) {
    priv->type = PROCFS_MAPS;
    priv->len = (int)procfs_maps_size(target);
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

  if (strcmp(rest, "exe") == 0) {
    if (nofollow) {
      kmfree(priv, sizeof(*priv));
      return -40;
    }
    priv->type = PROCFS_EXE_LINK;
    priv->len = 0;
    *private = priv;
    return 0;
  }

  if (strcmp(rest, "cwd") == 0) {
    if (nofollow) {
      kmfree(priv, sizeof(*priv));
      return -40;
    }
    priv->type = PROCFS_CWD_LINK;
    priv->len = 0;
    *private = priv;
    return 0;
  }

  {
    int fd = parse_procfs_fd_leaf(rest);
    struct open_file **ftable;
    if (fd >= 0) {
      ftable = proc_ofile(target);
      if (fd >= NOFILE || !ftable[fd]) {
        kmfree(priv, sizeof(*priv));
        return -2;
      }
      if (nofollow) {
        kmfree(priv, sizeof(*priv));
        return -40;
      }
      priv->type = PROCFS_FDLINK;
      priv->fd = fd;
      priv->len = 0;
      *private = priv;
      return 0;
    }
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

  if (priv->type == PROCFS_MAPS)
    return procfs_read_maps(priv, buf, count, offset);

  if (procfs_is_dir(priv->type))
    return -EISDIR;
  if (procfs_is_link(priv->type))
    return -EINVAL;

  if (*offset < 0)
    return -EINVAL;

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

static int procfs_reopen(void *fs_data, void *private, int flags,
                         void **new_private) {
  (void)fs_data;
  struct procfs_private *old = (struct procfs_private *)private;
  int flag_ret = procfs_check_read_open_flags(flags);
  if (flag_ret < 0)
    return flag_ret;
  if (!old)
    return -2; /* -ENOENT */

  struct procfs_private *priv =
    (struct procfs_private *)kmalloc(sizeof(struct procfs_private));
  if (!priv)
    return -12; /* -ENOMEM */
  /* BF: /proc/self/fd/N must be able to reopen procfs pseudo-files without
   * falling back to path lookup, while still giving the new fd a fresh offset. */
  memcpy(priv, old, sizeof(*priv));
  *new_private = priv;
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

  #define STAT_FILE(ino_val, size_val) do { \
    uint32_t mode = (uint32_t)(0x8000 | 0444); \
    uint64_t ino = (ino_val); \
    uint64_t file_size = (uint64_t)(size_val); \
    memcpy(st + 8, &ino, 8); \
    memcpy(st + 16, &mode, 4); \
    memcpy(st + 48, &file_size, 8); \
    return 0; \
  } while(0)

  #define STAT_RENDERED_FILE(ino_val, type_val, target_val) do { \
    uint64_t rendered_size = 0; \
    int size_ret = procfs_rendered_size((type_val), (target_val), &rendered_size); \
    if (size_ret < 0) return size_ret; \
    STAT_FILE((ino_val), rendered_size); \
  } while(0)

  #define STAT_LNK(ino_val, size_val) do { \
    uint32_t mode = (uint32_t)(0xA000 | 0777); \
    uint64_t ino = (ino_val); \
    uint64_t sz = (uint64_t)(size_val); \
    memcpy(st + 8, &ino, 8); \
    memcpy(st + 16, &mode, 4); \
    memcpy(st + 48, &sz, 8); \
    return 0; \
  } while(0)

  if (*p == '\0') { STAT_DIR(200); }
  if (strcmp(p, "dmesg") == 0)    { STAT_FILE(201, 0); }
  if (strcmp(p, "mounts") == 0)   { STAT_RENDERED_FILE(202, PROCFS_MOUNTS, 0); }
  if (strcmp(p, "cpuinfo") == 0)  { STAT_RENDERED_FILE(203, PROCFS_CPUINFO, 0); }
  if (strcmp(p, "version") == 0)  { STAT_RENDERED_FILE(204, PROCFS_VERSION, 0); }
  if (strcmp(p, "uptime") == 0)   { STAT_RENDERED_FILE(205, PROCFS_UPTIME, 0); }
  if (strcmp(p, "meminfo") == 0)  { STAT_RENDERED_FILE(206, PROCFS_MEMINFO, 0); }

  /* /proc/self — symlink to current PID */
  pid_t self_pid = current_proc ? current_proc->pid : 1;
  if (strcmp(p, "self") == 0) {
    char tmp[16];
    int len = append_int(tmp, sizeof(tmp), 0, self_pid);
    STAT_LNK(207, len);
  }
  if (strncmp(p, "self/", 5) == 0) {
    /* Delegate to numeric pid path */
    p += 5;
    /* BF: Mirror procfs_open's live-task handling so self stat/lookups do not
     * fail just because the current child is transient in the proc table. */
    struct proc *target = procfs_find_target(self_pid);
    if (!target) return -2;
    if (*p == '\0') { STAT_DIR((uint64_t)(300 + self_pid)); }
    {
      enum procfs_type type;
      if (procfs_pid_file_type(p, &type) == 0) {
        STAT_RENDERED_FILE(procfs_pid_file_ino(self_pid, type), type, target);
      }
    }
    if (strcmp(p, "exe") == 0) {
      STAT_LNK((uint64_t)(2000 + self_pid * 10 + 1),
               procfs_link_target_len(target, PROCFS_EXE_LINK, -1));
    }
    if (strcmp(p, "cwd") == 0) {
      STAT_LNK((uint64_t)(2000 + self_pid * 10 + 2),
               procfs_link_target_len(target, PROCFS_CWD_LINK, -1));
    }
    if (strcmp(p, "fd") == 0) { STAT_DIR((uint64_t)(400 + self_pid)); }
    {
      int fd = parse_procfs_fd_leaf(p);
      if (fd >= 0 && fd < NOFILE && proc_ofile(target)[fd]) {
        STAT_LNK((uint64_t)(5000 + self_pid * NOFILE + fd),
                 procfs_link_target_len(target, PROCFS_FDLINK, fd));
      }
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

  struct proc *target = procfs_find_target((pid_t)pid);
  if (!target) return -2;

  const char *rest = p + i;
  while (*rest == '/') rest++;

  if (*rest == '\0') { STAT_DIR((uint64_t)(300 + pid)); }

  if (strcmp(rest, "fd") == 0) { STAT_DIR((uint64_t)(400 + pid)); }

  {
    enum procfs_type type;
    if (procfs_pid_file_type(rest, &type) == 0) {
      STAT_RENDERED_FILE(procfs_pid_file_ino((pid_t)pid, type), type, target);
    }
  }

  if (strcmp(rest, "exe") == 0) {
    STAT_LNK((uint64_t)(2000 + pid * 10 + 1),
             procfs_link_target_len(target, PROCFS_EXE_LINK, -1));
  }

  if (strcmp(rest, "cwd") == 0) {
    STAT_LNK((uint64_t)(2000 + pid * 10 + 2),
             procfs_link_target_len(target, PROCFS_CWD_LINK, -1));
  }

  {
    int fd = parse_procfs_fd_leaf(rest);
    if (fd >= 0 && fd < NOFILE && proc_ofile(target)[fd]) {
      STAT_LNK((uint64_t)(5000 + pid * NOFILE + fd),
               procfs_link_target_len(target, PROCFS_FDLINK, fd));
    }
  }

  #undef STAT_DIR
  #undef STAT_FILE
  #undef STAT_RENDERED_FILE
  #undef STAT_LNK

  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* procfs_fstat                                                        */
/* ------------------------------------------------------------------ */
static int procfs_fstat(void *fs_data, void *private, uint8_t *st) {
  (void)fs_data;
  struct procfs_private *priv = (struct procfs_private *)private;
  if (!priv)
    return -2;
  memset(st, 0, 128);
  uint64_t dev = 4;
  uint32_t nlink = 1;
  int32_t blksz = 4096;
  memcpy(st + 0, &dev, 8);
  memcpy(st + 20, &nlink, 4);
  memcpy(st + 56, &blksz, 4);

  uint32_t dir_mode = (uint32_t)(0x4000 | 0555);
  uint32_t reg_mode = (uint32_t)(0x8000 | 0444);
  uint32_t lnk_mode = (uint32_t)(0xA000 | 0777);

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
  case PROCFS_FDLINK:
    { struct proc *target = procfs_find_target(priv->pid);
      uint64_t ino=(uint64_t)(5000 + priv->pid * NOFILE + priv->fd);
      uint64_t sz=(uint64_t)procfs_link_target_len(target, PROCFS_FDLINK, priv->fd);
      memcpy(st+8,&ino,8); memcpy(st+16,&lnk_mode,4); memcpy(st+48,&sz,8); break; }
  case PROCFS_EXE_LINK:
    { struct proc *target = procfs_find_target(priv->pid);
      uint64_t ino=(uint64_t)(2000 + priv->pid * 10 + 1);
      uint64_t sz=(uint64_t)procfs_link_target_len(target, PROCFS_EXE_LINK, -1);
      memcpy(st+8,&ino,8); memcpy(st+16,&lnk_mode,4); memcpy(st+48,&sz,8); break; }
  case PROCFS_CWD_LINK:
    { struct proc *target = procfs_find_target(priv->pid);
      uint64_t ino=(uint64_t)(2000 + priv->pid * 10 + 2);
      uint64_t sz=(uint64_t)procfs_link_target_len(target, PROCFS_CWD_LINK, -1);
      memcpy(st+8,&ino,8); memcpy(st+16,&lnk_mode,4); memcpy(st+48,&sz,8); break; }
  case PROCFS_STATUS:
  case PROCFS_CMDLINE:
  case PROCFS_MAPS:
  case PROCFS_STAT_FILE:
    { struct proc *target = procfs_find_target(priv->pid);
      uint64_t ino=procfs_pid_file_ino(priv->pid, priv->type);
      uint64_t sz= priv->type == PROCFS_MAPS && target
                 ? procfs_maps_size(target)
                 : (uint64_t)priv->len;
      memcpy(st+8,&ino,8); memcpy(st+16,&reg_mode,4); memcpy(st+48,&sz,8); break; }
  default:
    return -2;
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

  if (!procfs_is_dir(priv->type))
    return -ENOTDIR;

  #define EMIT_DIRENT(name_str, ino_val, dtype) do { \
    if (seen >= skip) { \
      int nl = strlen(name_str); \
      int rl = (19 + nl + 1 + 7) & ~7; \
      if (written + (unsigned int)rl > count) { \
        /* BF: A buffer too small for one record must not consume the dir offset. */ \
        if (written == 0) return -EINVAL; \
        goto done; \
      } \
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
    EMIT_DIRENT("status",  procfs_pid_file_ino(priv->pid, PROCFS_STATUS), 8);
    EMIT_DIRENT("cmdline", procfs_pid_file_ino(priv->pid, PROCFS_CMDLINE), 8);
    EMIT_DIRENT("maps",    procfs_pid_file_ino(priv->pid, PROCFS_MAPS), 8);
    EMIT_DIRENT("stat",    procfs_pid_file_ino(priv->pid, PROCFS_STAT_FILE), 8);
    EMIT_DIRENT("fd",      (uint64_t)(400 + priv->pid), 4);
    EMIT_DIRENT("exe",     (uint64_t)(2000 + priv->pid * 10 + 1), 10); /* DT_LNK */
    EMIT_DIRENT("cwd",     (uint64_t)(2000 + priv->pid * 10 + 2), 10); /* DT_LNK */
  } else if (priv->type == PROCFS_FDDIR) {
    /* List open file descriptors */
    uint64_t base = (uint64_t)(400 + priv->pid);
    EMIT_DIRENT(".", base, 4);
    EMIT_DIRENT("..", (uint64_t)(300 + priv->pid), 4);
    struct proc *p = procfs_find_target(priv->pid);
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
          EMIT_DIRENT(fdname, (uint64_t)(5000 + priv->pid * NOFILE + fd), 10); /* DT_LNK */
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
    if (pid < 0) {
      /* BF: Existing non-link procfs nodes fail readlink with EINVAL, while
       * absent root entries fail with ENOENT.  Keep those errno values
       * distinct so readlink("/proc/no-such") is not reported as a type
       * error on an existing node. */
      if (*p == '\0' || strcmp(p, "dmesg") == 0 ||
          strcmp(p, "mounts") == 0 || strcmp(p, "cpuinfo") == 0 ||
          strcmp(p, "version") == 0 || strcmp(p, "uptime") == 0 ||
          strcmp(p, "meminfo") == 0)
        return -22; /* EINVAL */
      return -2; /* ENOENT */
    }
    target_pid = (pid_t)pid;
    rest = p + i;
    while (*rest == '/') rest++;
  }

  /* BF: /proc/self/{exe,cwd} must resolve against the live caller, not only
   * whatever is presently discoverable through the proc table. */
  struct proc *target = procfs_find_target(target_pid);
  if (!target) return -2; /* ENOENT */

  /* BF: /proc/<pid>/fd/<n> directory entries are symlinks; readlink should
   * expose the same stored file path used for dirfd-relative resolution. */
  if (strncmp(rest, "fd/", 3) == 0) {
    int fd = parse_procfs_fd_leaf(rest);
    if (fd < 0 || fd >= NOFILE)
      return -2;
    struct open_file **ftable = proc_ofile(target);
    struct open_file *f = ftable[fd];
    if (!f)
      return -2;

    int len = 0;
    if (f->path[0] != '\0') {
      len = strlen(f->path);
      if (len > bufsiz) len = bufsiz;
      memcpy(buf, f->path, len);
      return len;
    } else if (f->is_pipe) {
      char tmp[64];
      len = append_str(tmp, sizeof(tmp), len, "pipe:[");
      len = append_int(tmp, sizeof(tmp), len, f->pipe_id);
      if (len < (int)sizeof(tmp)) tmp[len++] = ']';
      if (len > bufsiz) len = bufsiz;
      memcpy(buf, tmp, len);
      return len;
    } else {
      char tmp[64];
      len = append_str(tmp, sizeof(tmp), len, "anon:[");
      len = append_int(tmp, sizeof(tmp), len, fd);
      if (len < (int)sizeof(tmp)) tmp[len++] = ']';
      if (len > bufsiz) len = bufsiz;
      memcpy(buf, tmp, len);
      return len;
    }
  }

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

  if (*rest != '\0' && strcmp(rest, "status") != 0 &&
      strcmp(rest, "cmdline") != 0 && strcmp(rest, "maps") != 0 &&
      strcmp(rest, "stat") != 0 && strcmp(rest, "fd") != 0)
    return -2; /* ENOENT */

  return -22; /* EINVAL — not a symlink */
}

/* ------------------------------------------------------------------ */
/* procfs VFS ops struct                                               */
/* ------------------------------------------------------------------ */
struct vfs_ops procfs_ops = {
  .open     = procfs_open,
  .read     = procfs_read,
  .write    = procfs_write,
  .ftruncate = NULL,
  .reopen   = procfs_reopen,
  .close    = procfs_close,
  .stat     = procfs_stat,
  .fstat    = procfs_fstat,
  .readdir  = procfs_readdir,
  .mkdir    = procfs_mkdir,
  .unlink   = procfs_unlink,
  .rmdir    = NULL,
  .readlink = procfs_readlink,
};
