#include <vfs.h>
#include <string.h>
#include <printk.h>
#include <uart.h>
#include <kmalloc.h>
#include <proc.h>
#include <tty.h>
#include <errno.h>

#define O_NOCTTY 0400

/* ------------------------------------------------------------------ */
/* Device table                                                         */
/* ------------------------------------------------------------------ */
#define MAX_DEVICES 8

struct device {
  const char *name;
  int64_t (*read_fn)(char *buf, uint64_t count);
  int64_t (*write_fn)(const char *buf, uint64_t count);
  int is_tty;     /* 1 if this device is a terminal */
};

#define PTY_BUF_SIZE 512
#define PTY_ROLE_NONE   0
#define PTY_ROLE_MASTER 1
#define PTY_ROLE_SLAVE  2

struct pty_state {
  int active;
  int master_refs;
  int slave_refs;
  struct termios termios;
  pid_t sid;
  pid_t fg_pgid;
  char master_to_slave[PTY_BUF_SIZE];
  int m2s_r;
  int m2s_w;
  char slave_to_master[PTY_BUF_SIZE];
  int s2m_r;
  int s2m_w;
};

static struct pty_state pty0;

/* /dev/null */
static int64_t devnull_read(char *buf, uint64_t count) {
  (void)buf; (void)count;
  return 0;
}
static int64_t devnull_write(const char *buf, uint64_t count) {
  (void)buf;
  return (int64_t)count;
}

/* /dev/zero */
static int64_t devzero_read(char *buf, uint64_t count) {
  memset(buf, 0, count);
  return (int64_t)count;
}
static int64_t devzero_write(const char *buf, uint64_t count) {
  (void)buf;
  return (int64_t)count;
}

/* /dev/console */
static int64_t devconsole_read(char *buf, uint64_t count) {
  return (int64_t)tty_read(buf, (int)count);
}
static int64_t devconsole_write(const char *buf, uint64_t count) {
  for (uint64_t i = 0; i < count; i++)
    uart_write(buf[i]);
  return (int64_t)count;
}

/* /dev/urandom — PCG-based pseudorandom bytes */
static uint64_t urandom_state;
static int urandom_seeded;

static int64_t devurandom_read(char *buf, uint64_t count) {
  if (!urandom_seeded) {
    /* Seed from RISC-V cycle counter */
    asm volatile("rdtime %0" : "=r"(urandom_state));
    urandom_seeded = 1;
  }
  for (uint64_t i = 0; i < count; i++) {
    urandom_state = urandom_state * 6364136223846793005ULL + 1442695040888963407ULL;
    buf[i] = (char)(urandom_state >> 33);
  }
  return (int64_t)count;
}

static int64_t devurandom_write(const char *buf, uint64_t count) {
  /* Writing to urandom mixes in entropy (just XOR into state) */
  for (uint64_t i = 0; i < count; i++)
    urandom_state ^= (uint64_t)buf[i] << ((i % 8) * 8);
  return (int64_t)count;
}

static struct device devices[MAX_DEVICES] = {
  { "null",    devnull_read,    devnull_write,    0 },
  { "zero",    devzero_read,    devzero_write,    0 },
  { "console", devconsole_read, devconsole_write, 1 },
  { "tty",     devconsole_read, devconsole_write, 1 },
  { "ptmx",    NULL,            NULL,             1 },
  { "urandom", devurandom_read, devurandom_write, 0 },
  { "random",  devurandom_read, devurandom_write, 0 },
};
static int num_devices = 7;

/* ------------------------------------------------------------------ */
/* devfs private data                                                   */
/* ------------------------------------------------------------------ */
struct devfs_private {
  struct device *dev;
  int is_global_tty;
  struct termios tty_termios;
  pid_t tty_sid;
  pid_t tty_fg_pgid;
  struct pty_state *pty;
  int pty_role;
};

static void init_devfs_termios(struct termios *t) {
  memset(t, 0, sizeof(*t));
  t->c_iflag = ICRNL;
  t->c_oflag = OPOST | ONLCR;
  t->c_cflag = 0;
  t->c_lflag = ICANON | ECHO | ISIG;
  t->c_cc[VINTR] = 0x03;
  t->c_cc[VQUIT] = 0x1C;
  t->c_cc[VERASE] = 0x7F;
  t->c_cc[VEOF] = 0x04;
  t->c_cc[VSUSP] = 0x1A;
  t->c_cc[VMIN] = 1;
  t->c_cc[VTIME] = 0;
}

static int process_group_is_orphaned(pid_t sid, pid_t pgid) {
  int found = 0;
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED || p->state == ZOMBIE ||
        p->sid != sid || p->pgid != pgid)
      continue;
    found = 1;
    if (p->parent &&
        p->parent->state != UNUSED &&
        p->parent->state != ZOMBIE &&
        p->parent->sid == sid &&
        p->parent->pgid != pgid)
      return 0;
  }
  return found;
}

static int ring_avail(int r, int w) {
  return (w - r + PTY_BUF_SIZE) % PTY_BUF_SIZE;
}

static int ring_free(int r, int w) {
  return PTY_BUF_SIZE - 1 - ring_avail(r, w);
}

static int ring_put(char *buf, int *w, int r, char c) {
  if (ring_free(r, *w) == 0)
    return -1;
  buf[*w] = c;
  *w = (*w + 1) % PTY_BUF_SIZE;
  return 0;
}

static int ring_get(char *buf, int *r, int w, char *out) {
  if (*r == w)
    return -1;
  *out = buf[*r];
  *r = (*r + 1) % PTY_BUF_SIZE;
  return 0;
}

static void reset_pty(struct pty_state *pty) {
  memset(pty, 0, sizeof(*pty));
  pty->active = 1;
  init_devfs_termios(&pty->termios);
}

static int64_t pty_read(struct devfs_private *priv, char *buf, uint64_t count) {
  struct pty_state *pty = priv->pty;
  int *r;
  int *w;
  char *ring;
  uint64_t n = 0;

  if (!pty)
    return -1;

  if (priv->pty_role == PTY_ROLE_MASTER) {
    ring = pty->slave_to_master;
    r = &pty->s2m_r;
    w = &pty->s2m_w;
  } else {
    ring = pty->master_to_slave;
    r = &pty->m2s_r;
    w = &pty->m2s_w;
  }

  while (n < count && ring_get(ring, r, *w, &buf[n]) == 0)
    n++;
  return (int64_t)n;
}

static int64_t pty_write(struct devfs_private *priv, const char *buf, uint64_t count) {
  struct pty_state *pty = priv->pty;
  int *r;
  int *w;
  char *ring;
  uint64_t n = 0;

  if (!pty)
    return -1;

  if (priv->pty_role == PTY_ROLE_MASTER) {
    ring = pty->master_to_slave;
    r = &pty->m2s_r;
    w = &pty->m2s_w;
  } else {
    ring = pty->slave_to_master;
    r = &pty->s2m_r;
    w = &pty->s2m_w;
  }

  while (n < count && ring_put(ring, w, *r, buf[n]) == 0)
    n++;
  return (int64_t)n;
}

/* ------------------------------------------------------------------ */
/* devfs_open                                                           */
/* ------------------------------------------------------------------ */
static int devfs_open(void *fs_data, const char *path, int flags, void **private) {
  (void)fs_data;
  (void)flags;

  /* path is relative to /dev mount, e.g. "null", "zero", "console"
   * or "/null", "/zero", "/console" if relpath includes leading / */
  const char *name = path;
  while (*name == '/') name++;

  /* If path is empty or "/", treat as opening /dev directory itself */
  if (*name == '\0') {
    /* Allocate private with NULL dev to indicate directory */
    struct devfs_private *priv = (struct devfs_private *)kmalloc(sizeof(struct devfs_private));
    if (!priv) return -12; /* -ENOMEM */
    memset(priv, 0, sizeof(*priv));
    priv->dev = NULL;
    *private = priv;
    return 0;
  }

  if (strcmp(name, "pts/0") == 0) {
    struct devfs_private *priv;
    if (!pty0.active)
      return -2;
    priv = (struct devfs_private *)kmalloc(sizeof(struct devfs_private));
    if (!priv) return -12;
    memset(priv, 0, sizeof(*priv));
    priv->dev = &devices[4]; /* ptmx backing device */
    priv->pty = &pty0;
    priv->pty_role = PTY_ROLE_SLAVE;
    pty0.slave_refs++;
    if (!(flags & O_NOCTTY) &&
        current_proc &&
        current_proc->pid == current_proc->sid &&
        pty0.sid == 0) {
      pty0.sid = current_proc->sid;
      pty0.fg_pgid = current_proc->pgid;
    }
    *private = priv;
    return 0;
  }

  for (int i = 0; i < num_devices; i++) {
    if (strcmp(devices[i].name, name) == 0) {
      struct devfs_private *priv = (struct devfs_private *)kmalloc(sizeof(struct devfs_private));
      if (!priv) return -12; /* -ENOMEM */
      memset(priv, 0, sizeof(*priv));
      priv->dev = &devices[i];
      if (strcmp(devices[i].name, "ptmx") == 0) {
        reset_pty(&pty0);
        priv->pty = &pty0;
        priv->pty_role = PTY_ROLE_MASTER;
        pty0.master_refs = 1;
        *private = priv;
        return 0;
      }
      if (devices[i].is_tty) {
        init_devfs_termios(&priv->tty_termios);
        priv->is_global_tty = strcmp(devices[i].name, "tty") == 0;
        if (!priv->is_global_tty &&
            !(flags & O_NOCTTY) &&
            current_proc &&
            current_proc->pid == current_proc->sid) {
          priv->tty_sid = current_proc->sid;
          priv->tty_fg_pgid = current_proc->pgid;
        }
      }
      *private = priv;
      return 0;
    }
  }
  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* devfs_read                                                           */
/* ------------------------------------------------------------------ */
static int64_t devfs_read(void *fs_data, void *private, char *buf,
                          uint64_t count, int64_t *offset) {
  (void)fs_data;
  (void)offset;
  struct devfs_private *priv = (struct devfs_private *)private;
  if (priv && priv->pty)
    return pty_read(priv, buf, count);
  if (!priv || !priv->dev || !priv->dev->read_fn)
    return 0;
  return priv->dev->read_fn(buf, count);
}

/* ------------------------------------------------------------------ */
/* devfs_write                                                          */
/* ------------------------------------------------------------------ */
static int64_t devfs_write(void *fs_data, void *private, const char *buf,
                           uint64_t count, int64_t *offset) {
  (void)fs_data;
  (void)offset;
  struct devfs_private *priv = (struct devfs_private *)private;
  if (priv && priv->pty)
    return pty_write(priv, buf, count);
  if (!priv || !priv->dev || !priv->dev->write_fn)
    return -1;
  return priv->dev->write_fn(buf, count);
}

/* ------------------------------------------------------------------ */
/* devfs_close                                                          */
/* ------------------------------------------------------------------ */
static int devfs_close(void *fs_data, void *private) {
  (void)fs_data;
  if (private) {
    struct devfs_private *priv = (struct devfs_private *)private;
    if (priv->pty) {
      if (priv->pty_role == PTY_ROLE_MASTER && priv->pty->master_refs > 0)
        priv->pty->master_refs--;
      if (priv->pty_role == PTY_ROLE_SLAVE && priv->pty->slave_refs > 0)
        priv->pty->slave_refs--;
    }
    kmfree(private, sizeof(struct devfs_private));
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* devfs_fstat -- per-fd stat for open device files                     */
/* ------------------------------------------------------------------ */
static int devfs_fstat(void *fs_data, void *private, uint8_t *st) {
  (void)fs_data;
  struct devfs_private *priv = (struct devfs_private *)private;
  memset(st, 0, 128);
  uint64_t dev = 3; /* devfs device */
  uint32_t nlink = 1;
  int32_t blksz = 4096;
  memcpy(st + 0, &dev, 8);
  memcpy(st + 20, &nlink, 4);
  memcpy(st + 56, &blksz, 4);
  if (priv && priv->dev) {
    uint32_t mode = (uint32_t)(0x2000 | 0666); /* S_IFCHR | 0666 */
    /* Find device index for rdev/ino */
    uint64_t idx = 1;
    for (int i = 0; i < num_devices; i++) {
      if (&devices[i] == priv->dev) { idx = (uint64_t)(i + 1); break; }
    }
    uint64_t rdev = idx;
    memcpy(st + 8, &idx, 8);   /* st_ino */
    memcpy(st + 16, &mode, 4);
    memcpy(st + 32, &rdev, 8); /* st_rdev */
  } else {
    /* /dev directory itself */
    uint32_t mode = (uint32_t)(0x4000 | 0755); /* S_IFDIR | 0755 */
    uint64_t ino = 100;
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* devfs_is_tty -- query whether an open devfs file is a terminal      */
/* ------------------------------------------------------------------ */
int devfs_is_tty(void *private) {
  struct devfs_private *priv = (struct devfs_private *)private;
  return (priv && ((priv->dev && priv->dev->is_tty) || priv->pty)) ? 1 : 0;
}

int devfs_tty_ioctl(void *private, unsigned long request, void *argp) {
  struct devfs_private *priv = (struct devfs_private *)private;
  if (!priv || (!priv->pty && (!priv->dev || !priv->dev->is_tty)))
    return -ENOTTY;

  if (priv->pty) {
    switch (request) {
    case TCGETS:
      memcpy(argp, &priv->pty->termios, sizeof(struct termios));
      return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
      memcpy(&priv->pty->termios, argp, sizeof(struct termios));
      return 0;
    case TIOCGSID:
      if (argp)
        *(int *)argp = priv->pty->sid;
      return priv->pty->sid;
    case TIOCGPGRP:
      if (argp)
        *(int *)argp = priv->pty->fg_pgid;
      return priv->pty->fg_pgid;
    case TIOCSPGRP: {
      pid_t pgid;
      if (!argp)
        return -EINVAL;
      pgid = *(int *)argp;
      if (pgid <= 0)
        return -EINVAL;
      priv->pty->fg_pgid = pgid;
      return 0;
    }
    default:
      return -ENOTTY;
    }
  }

  if (priv->is_global_tty) {
    if (request == TIOCGSID) {
      if (!current_proc)
        return -ENOTTY;
      if (argp)
        *(int *)argp = current_proc->sid;
      return current_proc->sid;
    }
    return tty_ioctl(request, argp);
  }

  switch (request) {
  case TCGETS:
    memcpy(argp, &priv->tty_termios, sizeof(struct termios));
    return 0;
  case TCSETS:
  case TCSETSW:
  case TCSETSF:
    memcpy(&priv->tty_termios, argp, sizeof(struct termios));
    return 0;
  case TIOCGSID:
    if (priv->tty_sid == 0 || !current_proc || current_proc->sid != priv->tty_sid)
      return -ENOTTY;
    if (argp)
      *(int *)argp = priv->tty_sid;
    return priv->tty_sid;
  case TIOCGPGRP:
    if (priv->tty_sid == 0 || !current_proc || current_proc->sid != priv->tty_sid)
      return -ENOTTY;
    if (argp)
      *(int *)argp = priv->tty_fg_pgid;
    return priv->tty_fg_pgid;
  case TIOCSPGRP: {
    pid_t pgid;

    if (priv->tty_sid == 0 || !current_proc || current_proc->sid != priv->tty_sid)
      return -ENOTTY;
    if (!argp)
      return -EINVAL;
    pgid = *(int *)argp;
    if (pgid <= 0)
      return -EINVAL;

    if (current_proc->pgid != priv->tty_fg_pgid &&
        process_group_is_orphaned(current_proc->sid, current_proc->pgid))
      return -EIO;

    priv->tty_fg_pgid = pgid;
    return 0;
  }
  default:
    return -ENOTTY;
  }
}

/* ------------------------------------------------------------------ */
/* devfs_stat                                                           */
/* ------------------------------------------------------------------ */
static int devfs_stat(void *fs_data, const char *path, uint8_t *st) {
  (void)fs_data;
  const char *name = path;
  while (*name == '/') name++;

  uint64_t dev = 3; /* devfs device */
  uint32_t nlink = 1;
  int32_t blksz = 4096;

  /* /dev directory itself */
  if (*name == '\0') {
    memset(st, 0, 128);
    uint32_t mode = (uint32_t)(0x4000 | 0755); /* S_IFDIR */
    uint64_t ino = 100;
    memcpy(st + 0, &dev, 8);
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    memcpy(st + 20, &nlink, 4);
    memcpy(st + 56, &blksz, 4);
    return 0;
  }

  if (strcmp(name, "pts/0") == 0 && pty0.active) {
    memset(st, 0, 128);
    memcpy(st + 0, &dev, 8);
    memcpy(st + 20, &nlink, 4);
    memcpy(st + 56, &blksz, 4);
    {
      uint32_t mode = (uint32_t)(0x2000 | 0666);
      uint64_t ino = 200;
      uint64_t rdev = 200;
      memcpy(st + 8, &ino, 8);
      memcpy(st + 16, &mode, 4);
      memcpy(st + 32, &rdev, 8);
    }
    return 0;
  }

  for (int i = 0; i < num_devices; i++) {
    if (strcmp(devices[i].name, name) == 0) {
      memset(st, 0, 128);
      uint32_t mode = (uint32_t)(0x2000 | 0666); /* S_IFCHR | 0666 */
      uint64_t ino = (uint64_t)(i + 1);
      uint64_t rdev = (uint64_t)(i + 1);
      memcpy(st + 0, &dev, 8);
      memcpy(st + 8, &ino, 8);
      memcpy(st + 16, &mode, 4);
      memcpy(st + 20, &nlink, 4);
      memcpy(st + 32, &rdev, 8);
      memcpy(st + 56, &blksz, 4);
      return 0;
    }
  }
  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* devfs_readdir                                                        */
/* ------------------------------------------------------------------ */
static int devfs_readdir(void *fs_data, void *private, char *ubuf,
                         unsigned int count, int64_t *offset) {
  (void)fs_data;
  struct devfs_private *priv = (struct devfs_private *)private;
  if (!priv)
    return -1;

  uint64_t skip = (uint64_t)*offset;
  unsigned int written = 0;
  uint64_t seen = 0;

  /* Synthetic "." */
  if (seen >= skip) {
    int nl = 1; /* "." */
    int rl = (19 + nl + 1 + 7) & ~7;
    if (written + (unsigned int)rl > count) goto done;
    char *d = ubuf + written;
    memset(d, 0, rl);
    uint64_t ino = 1;
    int64_t doff = (int64_t)(seen + 1);
    uint16_t rlen = (uint16_t)rl;
    uint8_t dt = 4; /* DT_DIR */
    memcpy(d + 0, &ino, 8);
    memcpy(d + 8, &doff, 8);
    memcpy(d + 16, &rlen, 2);
    memcpy(d + 18, &dt, 1);
    d[19] = '.'; d[20] = '\0';
    written += (unsigned int)rl;
    *offset = (int64_t)(seen + 1);
  }
  seen++;

  /* Synthetic ".." */
  if (seen >= skip) {
    int nl = 2; /* ".." */
    int rl = (19 + nl + 1 + 7) & ~7;
    if (written + (unsigned int)rl > count) goto done;
    char *d = ubuf + written;
    memset(d, 0, rl);
    uint64_t ino = 1;
    int64_t doff = (int64_t)(seen + 1);
    uint16_t rlen = (uint16_t)rl;
    uint8_t dt = 4;
    memcpy(d + 0, &ino, 8);
    memcpy(d + 8, &doff, 8);
    memcpy(d + 16, &rlen, 2);
    memcpy(d + 18, &dt, 1);
    d[19] = '.'; d[20] = '.'; d[21] = '\0';
    written += (unsigned int)rl;
    *offset = (int64_t)(seen + 1);
  }
  seen++;

  /* Device entries */
  for (int i = 0; i < num_devices; i++) {
    if (seen >= skip) {
      int nl = strlen(devices[i].name);
      int rl = (19 + nl + 1 + 7) & ~7;
      if (written + (unsigned int)rl > count) break;
      char *d = ubuf + written;
      memset(d, 0, rl);
      uint64_t ino = (uint64_t)(i + 100);
      int64_t doff = (int64_t)(seen + 1);
      uint16_t rlen = (uint16_t)rl;
      uint8_t dt = 2; /* DT_CHR */
      memcpy(d + 0, &ino, 8);
      memcpy(d + 8, &doff, 8);
      memcpy(d + 16, &rlen, 2);
      memcpy(d + 18, &dt, 1);
      memcpy(d + 19, devices[i].name, nl + 1);
      written += (unsigned int)rl;
      *offset = (int64_t)(seen + 1);
    }
    seen++;
  }

done:
  return (int)written;
}

/* ------------------------------------------------------------------ */
/* devfs stubs                                                          */
/* ------------------------------------------------------------------ */
static int devfs_mkdir(void *fs_data, const char *path, int mode) {
  (void)fs_data;
  (void)path; (void)mode;
  return -30; /* -EROFS */
}

static int devfs_unlink(void *fs_data, const char *path) {
  (void)fs_data;
  (void)path;
  return -30; /* -EROFS */
}

/* ------------------------------------------------------------------ */
/* devfs VFS ops struct                                                 */
/* ------------------------------------------------------------------ */
struct vfs_ops devfs_ops = {
  .open    = devfs_open,
  .read    = devfs_read,
  .write   = devfs_write,
  .ftruncate = NULL,
  .close   = devfs_close,
  .stat    = devfs_stat,
  .fstat   = devfs_fstat,
  .readdir = devfs_readdir,
  .mkdir   = devfs_mkdir,
  .unlink  = devfs_unlink,
  .rmdir   = NULL,
};
