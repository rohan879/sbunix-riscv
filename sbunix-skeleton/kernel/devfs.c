#include <vfs.h>
#include <string.h>
#include <printk.h>
#include <uart.h>
#include <kmalloc.h>
#include <proc.h>

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
  /* UART raw byte delivery.  Returns one character at a time.
   * Sleeps via swtch() when no input is available -- this creates
   * a dependency on the process layer, acceptable in a teaching OS. */
  uint64_t i = 0;
  while (i < count) {
    char c;
    /* Wait until a character is available */
    while (uart_dequeue(&c) != 0) {
      /* Poll hardware UART directly in case interrupt missed a char */
      int hw_c = uart_getc();
      if (hw_c != -1) {
        uart_enqueue((char)hw_c);
        continue;
      }
      /* Sleep until UART interrupt wakes us */
      current_proc->waiting_for_uart = 1;
      current_proc->state = SLEEPING;
      swtch(&current_proc->context, &scheduler_context);
    }
    if (c == '\r') c = '\n';
    buf[i++] = c;
    /* Return after each character so the caller can process it */
    break;
  }
  return (int64_t)i;
}
static int64_t devconsole_write(const char *buf, uint64_t count) {
  for (uint64_t i = 0; i < count; i++)
    uart_write(buf[i]);
  return (int64_t)count;
}

static struct device devices[MAX_DEVICES] = {
  { "null",    devnull_read,    devnull_write,    0 },
  { "zero",    devzero_read,    devzero_write,    0 },
  { "console", devconsole_read, devconsole_write, 1 },
};
static int num_devices = 3;

/* ------------------------------------------------------------------ */
/* devfs private data                                                   */
/* ------------------------------------------------------------------ */
struct devfs_private {
  struct device *dev;
};

/* ------------------------------------------------------------------ */
/* devfs_open                                                           */
/* ------------------------------------------------------------------ */
static int devfs_open(const char *path, int flags, void **private) {
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
    priv->dev = NULL;
    *private = priv;
    return 0;
  }

  for (int i = 0; i < num_devices; i++) {
    if (strcmp(devices[i].name, name) == 0) {
      struct devfs_private *priv = (struct devfs_private *)kmalloc(sizeof(struct devfs_private));
      if (!priv) return -12; /* -ENOMEM */
      priv->dev = &devices[i];
      *private = priv;
      return 0;
    }
  }
  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* devfs_read                                                           */
/* ------------------------------------------------------------------ */
static int64_t devfs_read(void *private, char *buf, uint64_t count, int64_t *offset) {
  (void)offset;
  struct devfs_private *priv = (struct devfs_private *)private;
  if (!priv || !priv->dev || !priv->dev->read_fn)
    return 0;
  return priv->dev->read_fn(buf, count);
}

/* ------------------------------------------------------------------ */
/* devfs_write                                                          */
/* ------------------------------------------------------------------ */
static int64_t devfs_write(void *private, const char *buf, uint64_t count, int64_t *offset) {
  (void)offset;
  struct devfs_private *priv = (struct devfs_private *)private;
  if (!priv || !priv->dev || !priv->dev->write_fn)
    return -1;
  return priv->dev->write_fn(buf, count);
}

/* ------------------------------------------------------------------ */
/* devfs_close                                                          */
/* ------------------------------------------------------------------ */
static int devfs_close(void *private) {
  if (private)
    kmfree(private, sizeof(struct devfs_private));
  return 0;
}

/* ------------------------------------------------------------------ */
/* devfs_fstat -- per-fd stat for open device files                     */
/* ------------------------------------------------------------------ */
static int devfs_fstat(void *private, uint8_t *st) {
  struct devfs_private *priv = (struct devfs_private *)private;
  memset(st, 0, 128);
  if (priv && priv->dev) {
    uint32_t mode = (uint32_t)(0x2000 | 0666); /* S_IFCHR | 0666 */
    uint32_t nlink = 1;
    memcpy(st + 16, &mode, 4);
    memcpy(st + 20, &nlink, 4);
  } else {
    /* /dev directory itself */
    uint32_t mode = (uint32_t)(0x4000 | 0755); /* S_IFDIR | 0755 */
    uint32_t nlink = 1;
    memcpy(st + 16, &mode, 4);
    memcpy(st + 20, &nlink, 4);
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* devfs_is_tty -- query whether an open devfs file is a terminal      */
/* ------------------------------------------------------------------ */
int devfs_is_tty(void *private) {
  struct devfs_private *priv = (struct devfs_private *)private;
  return (priv && priv->dev && priv->dev->is_tty) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* devfs_stat                                                           */
/* ------------------------------------------------------------------ */
static int devfs_stat(const char *path, uint8_t *st) {
  const char *name = path;
  while (*name == '/') name++;

  /* /dev directory itself */
  if (*name == '\0') {
    memset(st, 0, 128);
    uint32_t mode = (uint32_t)(0x4000 | 0755); /* S_IFDIR */
    uint32_t nlink = 1;
    memcpy(st + 16, &mode, 4);
    memcpy(st + 20, &nlink, 4);
    return 0;
  }

  for (int i = 0; i < num_devices; i++) {
    if (strcmp(devices[i].name, name) == 0) {
      memset(st, 0, 128);
      uint32_t mode = (uint32_t)(0x2000 | 0666); /* S_IFCHR | 0666 */
      uint32_t nlink = 1;
      memcpy(st + 16, &mode, 4);
      memcpy(st + 20, &nlink, 4);
      return 0;
    }
  }
  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* devfs_readdir                                                        */
/* ------------------------------------------------------------------ */
static int devfs_readdir(void *private, char *ubuf, unsigned int count, int64_t *offset) {
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
static int devfs_mkdir(const char *path, int mode) {
  (void)path; (void)mode;
  return -30; /* -EROFS */
}

static int devfs_unlink(const char *path) {
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
  .close   = devfs_close,
  .stat    = devfs_stat,
  .fstat   = devfs_fstat,
  .readdir = devfs_readdir,
  .mkdir   = devfs_mkdir,
  .unlink  = devfs_unlink,
};
