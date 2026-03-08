/*
 * procfs.c — /proc virtual filesystem for SBUnix.
 *
 * Exposes per-PID directories with status and cmdline pseudo-files.
 * Read-only; follows the same VFS ops pattern as devfs.c.
 */

#include <vfs.h>
#include <string.h>
#include <printk.h>
#include <kmalloc.h>
#include <proc.h>

/* Types of procfs entries */
enum procfs_type {
  PROCFS_ROOT,      /* /proc directory */
  PROCFS_PIDDIR,    /* /proc/<pid> directory */
  PROCFS_STATUS,    /* /proc/<pid>/status */
  PROCFS_CMDLINE,   /* /proc/<pid>/cmdline */
};

struct procfs_private {
  enum procfs_type type;
  pid_t pid;
  char buf[512];    /* pre-rendered content for status/cmdline */
  int len;          /* length of content in buf */
};

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
    case SLEEPING: return "sleeping";
    case RUNNABLE: return "runnable";
    case RUNNING:  return "running";
    case ZOMBIE:   return "zombie";
    default:       return "unknown";
  }
}

/* Helper: snprintf-like into a fixed buffer (minimal, no varargs) */
static int procfs_render_status(char *buf, int bufsz, struct proc *p) {
  /* Build status line by line manually */
  int pos = 0;
  const char *st = state_name(p->state);
  pid_t ppid = p->parent ? p->parent->pid : 0;

  /* Name: */
  const char *h1 = "Name:\t";
  int l = strlen(h1);
  if (pos + l < bufsz) { memcpy(buf + pos, h1, l); pos += l; }
  l = strlen(p->name);
  if (pos + l < bufsz) { memcpy(buf + pos, p->name, l); pos += l; }
  if (pos < bufsz) buf[pos++] = '\n';

  /* State: */
  const char *h2 = "State:\t";
  l = strlen(h2);
  if (pos + l < bufsz) { memcpy(buf + pos, h2, l); pos += l; }
  l = strlen(st);
  if (pos + l < bufsz) { memcpy(buf + pos, st, l); pos += l; }
  if (pos < bufsz) buf[pos++] = '\n';

  /* Pid: (manual int-to-string) */
  const char *h3 = "Pid:\t";
  l = strlen(h3);
  if (pos + l < bufsz) { memcpy(buf + pos, h3, l); pos += l; }
  char num[12];
  int ni = 0;
  {
    int v = p->pid;
    if (v == 0) { num[ni++] = '0'; }
    else { char tmp[12]; int ti = 0; while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; } while (ti > 0) num[ni++] = tmp[--ti]; }
  }
  if (pos + ni < bufsz) { memcpy(buf + pos, num, ni); pos += ni; }
  if (pos < bufsz) buf[pos++] = '\n';

  /* PPid: */
  const char *h4 = "PPid:\t";
  l = strlen(h4);
  if (pos + l < bufsz) { memcpy(buf + pos, h4, l); pos += l; }
  ni = 0;
  {
    int v = ppid;
    if (v == 0) { num[ni++] = '0'; }
    else { char tmp[12]; int ti = 0; while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; } while (ti > 0) num[ni++] = tmp[--ti]; }
  }
  if (pos + ni < bufsz) { memcpy(buf + pos, num, ni); pos += ni; }
  if (pos < bufsz) buf[pos++] = '\n';

  /* Uid/Gid: */
  const char *h5 = "Uid:\t0\nGid:\t0\n";
  l = strlen(h5);
  if (pos + l < bufsz) { memcpy(buf + pos, h5, l); pos += l; }

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

  /* Extract PID component */
  char pidstr[16];
  int i = 0;
  while (p[i] && p[i] != '/' && i < 15) { pidstr[i] = p[i]; i++; }
  pidstr[i] = '\0';

  int pid = parse_pid(pidstr);
  if (pid < 0) { kmfree(priv, sizeof(*priv)); return -2; /* -ENOENT */ }

  struct proc *target = find_proc((pid_t)pid);
  if (!target) { kmfree(priv, sizeof(*priv)); return -2; }

  priv->pid = (pid_t)pid;

  /* Advance past PID */
  const char *rest = p + i;
  while (*rest == '/') rest++;

  if (*rest == '\0') {
    /* /proc/<pid> directory */
    priv->type = PROCFS_PIDDIR;
    *private = priv;
    return 0;
  }

  if (strcmp(rest, "status") == 0) {
    priv->type = PROCFS_STATUS;
    priv->len = procfs_render_status(priv->buf, (int)sizeof(priv->buf), target);
    *private = priv;
    return 0;
  }

  if (strcmp(rest, "cmdline") == 0) {
    priv->type = PROCFS_CMDLINE;
    int nl = strlen(target->name);
    if (nl > (int)sizeof(priv->buf) - 1) nl = (int)sizeof(priv->buf) - 1;
    memcpy(priv->buf, target->name, nl);
    priv->buf[nl] = '\0';
    priv->len = nl;
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

  if (priv->type != PROCFS_STATUS && priv->type != PROCFS_CMDLINE)
    return 0;

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

  if (*p == '\0') {
    /* /proc directory */
    uint32_t mode = (uint32_t)(0x4000 | 0555); /* S_IFDIR | r-xr-xr-x */
    uint64_t ino = 200;
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    return 0;
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

  if (*rest == '\0') {
    /* /proc/<pid> directory */
    uint32_t mode = (uint32_t)(0x4000 | 0555);
    uint64_t ino = (uint64_t)(300 + pid);
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    return 0;
  }

  if (strcmp(rest, "status") == 0 || strcmp(rest, "cmdline") == 0) {
    uint32_t mode = (uint32_t)(0x8000 | 0444); /* S_IFREG | r--r--r-- */
    uint64_t ino = (uint64_t)(1000 + pid * 10 + (rest[0] == 's' ? 1 : 2));
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    return 0;
  }

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

  if (priv->type == PROCFS_ROOT || priv->type == PROCFS_PIDDIR) {
    uint32_t mode = (uint32_t)(0x4000 | 0555);
    uint64_t ino = (priv->type == PROCFS_ROOT) ? 200 : (uint64_t)(300 + priv->pid);
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
  } else {
    uint32_t mode = (uint32_t)(0x8000 | 0444);
    uint64_t ino = (uint64_t)(1000 + priv->pid * 10 + (priv->type == PROCFS_STATUS ? 1 : 2));
    uint64_t size = (uint64_t)priv->len;
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    memcpy(st + 48, &size, 8); /* st_size */
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

  /* Helper macro to emit a dirent */
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
    /* List: . .. <pid1> <pid2> ... */
    EMIT_DIRENT(".", 200, 4);   /* DT_DIR */
    EMIT_DIRENT("..", 1, 4);    /* DT_DIR */

    for (struct proc *p = proc; p < &proc[64]; p++) {
      if (p->state != UNUSED) {
        /* Convert PID to string */
        char pidname[12];
        int ni = 0;
        int v = p->pid;
        if (v == 0) { pidname[ni++] = '0'; }
        else { char tmp[12]; int ti = 0; while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; } while (ti > 0) pidname[ni++] = tmp[--ti]; }
        pidname[ni] = '\0';
        EMIT_DIRENT(pidname, (uint64_t)(300 + p->pid), 4); /* DT_DIR */
      }
    }
  } else if (priv->type == PROCFS_PIDDIR) {
    /* List: . .. status cmdline */
    EMIT_DIRENT(".", (uint64_t)(300 + priv->pid), 4);
    EMIT_DIRENT("..", 200, 4);
    EMIT_DIRENT("status", (uint64_t)(1000 + priv->pid * 10 + 1), 8);  /* DT_REG */
    EMIT_DIRENT("cmdline", (uint64_t)(1000 + priv->pid * 10 + 2), 8); /* DT_REG */
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
/* procfs VFS ops struct                                               */
/* ------------------------------------------------------------------ */
struct vfs_ops procfs_ops = {
  .open    = procfs_open,
  .read    = procfs_read,
  .write   = procfs_write,
  .close   = procfs_close,
  .stat    = procfs_stat,
  .fstat   = procfs_fstat,
  .readdir = procfs_readdir,
  .mkdir   = procfs_mkdir,
  .unlink  = procfs_unlink,
  .rmdir   = NULL,
};
