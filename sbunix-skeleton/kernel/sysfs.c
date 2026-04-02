/*
 * sysfs.c — /sys virtual filesystem for SBUnix.
 *
 * Exposes kernel information and DTB device attributes.
 * Read-only; follows the same VFS ops pattern as procfs.c.
 */

#include <vfs.h>
#include <string.h>
#include <printk.h>
#include <kmalloc.h>
#include <dtb.h>
#include <virtio_blk.h>

/* Types of sysfs entries */
enum sysfs_type {
  SYSFS_ROOT,          /* /sys directory */
  SYSFS_KERNEL_DIR,    /* /sys/kernel directory */
  SYSFS_KERNEL_ATTR,   /* /sys/kernel/{version,hostname} */
  SYSFS_DEVICES_DIR,   /* /sys/devices directory */
  SYSFS_DEV_DIR,       /* /sys/devices/<name> directory */
  SYSFS_DEV_ATTR,      /* /sys/devices/<name>/<attr> */
};

struct sysfs_private {
  enum sysfs_type type;
  int dev_index;       /* index into dtb_devices.devices[] */
  char buf[256];       /* pre-rendered content */
  int len;             /* length of content in buf */
};

/* Helper: return type name prefix for a DTB device type */
static const char *dev_type_name(enum dtb_dev_type t) {
  switch (t) {
    case DTB_DEV_UART:        return "uart";
    case DTB_DEV_PLIC:        return "plic";
    case DTB_DEV_CLINT:       return "clint";
    case DTB_DEV_VIRTIO_MMIO: return "virtio";
    case DTB_DEV_PCI:         return "pci";
    default:                  return "unknown";
  }
}

/* Helper: count how many devices of the same type appear before index di */
static int dev_type_index(int di) {
  enum dtb_dev_type t = dtb_devices.devices[di].type;
  int count = 0;
  for (int i = 0; i < di; i++) {
    if (dtb_devices.devices[i].type == t)
      count++;
  }
  return count;
}

/* Helper: find device by name like "uart0", returns 0 on success */
static int find_device_by_name(const char *name, int *out_index) {
  /* Find where the numeric suffix starts */
  int prefix_len = 0;
  while (name[prefix_len] && (name[prefix_len] < '0' || name[prefix_len] > '9'))
    prefix_len++;
  if (prefix_len == 0 || !name[prefix_len])
    return -1;

  /* Parse the numeric suffix */
  int num = 0;
  for (int i = prefix_len; name[i]; i++) {
    if (name[i] < '0' || name[i] > '9') return -1;
    num = num * 10 + (name[i] - '0');
  }

  /* Match type prefix */
  enum dtb_dev_type target_type = DTB_DEV_UNKNOWN;
  if (prefix_len == 4 && memcmp(name, "uart", 4) == 0)
    target_type = DTB_DEV_UART;
  else if (prefix_len == 4 && memcmp(name, "plic", 4) == 0)
    target_type = DTB_DEV_PLIC;
  else if (prefix_len == 5 && memcmp(name, "clint", 5) == 0)
    target_type = DTB_DEV_CLINT;
  else if (prefix_len == 6 && memcmp(name, "virtio", 6) == 0)
    target_type = DTB_DEV_VIRTIO_MMIO;
  else if (prefix_len == 3 && memcmp(name, "pci", 3) == 0)
    target_type = DTB_DEV_PCI;
  else
    return -1;

  /* Find the Nth device of that type */
  int count = 0;
  for (int i = 0; i < dtb_devices.ndevices; i++) {
    if (dtb_devices.devices[i].type == target_type) {
      if (count == num) {
        *out_index = i;
        return 0;
      }
      count++;
    }
  }
  return -1;
}

/* Helper: format uint64_t as "0x..." hex string with newline, returns length */
static int fmt_hex64(char *buf, uint64_t val) {
  buf[0] = '0'; buf[1] = 'x';
  int pos = 2;
  if (val == 0) {
    buf[pos++] = '0';
  } else {
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
      int nibble = (int)((val >> i) & 0xf);
      if (nibble || started) {
        buf[pos++] = "0123456789abcdef"[nibble];
        started = 1;
      }
    }
  }
  buf[pos++] = '\n';
  return pos;
}

/* Helper: format uint64_t as decimal string with newline, returns length */
static int fmt_dec64(char *buf, uint64_t val) {
  if (val == 0) {
    buf[0] = '0'; buf[1] = '\n';
    return 2;
  }
  char tmp[20];
  int ti = 0;
  while (val > 0) {
    tmp[ti++] = '0' + (int)(val % 10);
    val /= 10;
  }
  int pos = 0;
  while (ti > 0)
    buf[pos++] = tmp[--ti];
  buf[pos++] = '\n';
  return pos;
}

/* ------------------------------------------------------------------ */
/* sysfs_open                                                          */
/* ------------------------------------------------------------------ */
static int sysfs_open(void *fs_data, const char *path, int flags, void **private) {
  (void)fs_data;
  (void)flags;
  const char *p = path;
  while (*p == '/') p++;

  struct sysfs_private *priv = (struct sysfs_private *)kmalloc(sizeof(struct sysfs_private));
  if (!priv) return -12; /* -ENOMEM */
  memset(priv, 0, sizeof(*priv));

  /* /sys root directory */
  if (*p == '\0') {
    priv->type = SYSFS_ROOT;
    *private = priv;
    return 0;
  }

  /* /sys/kernel */
  if (strcmp(p, "kernel") == 0) {
    priv->type = SYSFS_KERNEL_DIR;
    *private = priv;
    return 0;
  }

  /* /sys/kernel/version */
  if (strcmp(p, "kernel/version") == 0) {
    priv->type = SYSFS_KERNEL_ATTR;
    const char *ver = "SBUnix 1.0 riscv64\n";
    int l = strlen(ver);
    memcpy(priv->buf, ver, l);
    priv->len = l;
    *private = priv;
    return 0;
  }

  /* /sys/kernel/hostname */
  if (strcmp(p, "kernel/hostname") == 0) {
    priv->type = SYSFS_KERNEL_ATTR;
    const char *host = "sbunix\n";
    int l = strlen(host);
    memcpy(priv->buf, host, l);
    priv->len = l;
    *private = priv;
    return 0;
  }

  /* /sys/devices */
  if (strcmp(p, "devices") == 0) {
    priv->type = SYSFS_DEVICES_DIR;
    *private = priv;
    return 0;
  }

  /* /sys/devices/<name> or /sys/devices/<name>/<attr> */
  if (memcmp(p, "devices/", 8) == 0) {
    const char *rest = p + 8;
    /* Extract device name */
    char devname[32];
    int i = 0;
    while (rest[i] && rest[i] != '/' && i < 31) { devname[i] = rest[i]; i++; }
    devname[i] = '\0';

    int dev_idx;
    if (find_device_by_name(devname, &dev_idx) < 0) {
      kmfree(priv, sizeof(*priv));
      return -2; /* -ENOENT */
    }

    const char *attr = rest + i;
    while (*attr == '/') attr++;

    if (*attr == '\0') {
      /* /sys/devices/<name> directory */
      priv->type = SYSFS_DEV_DIR;
      priv->dev_index = dev_idx;
      *private = priv;
      return 0;
    }

    /* /sys/devices/<name>/<attr> */
    struct dtb_device *dev = &dtb_devices.devices[dev_idx];
    priv->type = SYSFS_DEV_ATTR;
    priv->dev_index = dev_idx;

    if (strcmp(attr, "address") == 0) {
      priv->len = fmt_hex64(priv->buf, dev->base_addr);
    } else if (strcmp(attr, "size") == 0) {
      priv->len = fmt_hex64(priv->buf, dev->size);
    } else if (strcmp(attr, "irq") == 0 && dev->irq > 0) {
      priv->len = fmt_dec64(priv->buf, (uint64_t)dev->irq);
    } else if (strcmp(attr, "compatible") == 0 && dev->compatible) {
      int cl = strlen(dev->compatible);
      if (cl > (int)sizeof(priv->buf) - 2) cl = (int)sizeof(priv->buf) - 2;
      memcpy(priv->buf, dev->compatible, cl);
      priv->buf[cl] = '\n';
      priv->len = cl + 1;
    } else if (strcmp(attr, "capacity") == 0 &&
               dev->type == DTB_DEV_VIRTIO_MMIO &&
               dev_type_index(dev_idx) == 0) {
      priv->len = fmt_dec64(priv->buf, virtio_blk_capacity());
    } else if (strcmp(attr, "sector_size") == 0 &&
               dev->type == DTB_DEV_VIRTIO_MMIO &&
               dev_type_index(dev_idx) == 0) {
      const char *ss = "512\n";
      memcpy(priv->buf, ss, 4);
      priv->len = 4;
    } else {
      kmfree(priv, sizeof(*priv));
      return -2; /* -ENOENT */
    }

    *private = priv;
    return 0;
  }

  kmfree(priv, sizeof(*priv));
  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* sysfs_read                                                          */
/* ------------------------------------------------------------------ */
static int64_t sysfs_read(void *fs_data, void *private, char *buf,
                          uint64_t count, int64_t *offset) {
  (void)fs_data;
  struct sysfs_private *priv = (struct sysfs_private *)private;
  if (!priv) return 0;

  /* Only file types have content */
  if (priv->type != SYSFS_KERNEL_ATTR && priv->type != SYSFS_DEV_ATTR)
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
/* sysfs_write                                                         */
/* ------------------------------------------------------------------ */
static int64_t sysfs_write(void *fs_data, void *private, const char *buf,
                           uint64_t count, int64_t *offset) {
  (void)fs_data; (void)private; (void)buf; (void)count; (void)offset;
  return -30; /* -EROFS */
}

/* ------------------------------------------------------------------ */
/* sysfs_close                                                         */
/* ------------------------------------------------------------------ */
static int sysfs_close(void *fs_data, void *private) {
  (void)fs_data;
  if (private)
    kmfree(private, sizeof(struct sysfs_private));
  return 0;
}

/* ------------------------------------------------------------------ */
/* sysfs_stat                                                          */
/* ------------------------------------------------------------------ */
static int sysfs_stat(void *fs_data, const char *path, uint8_t *st) {
  (void)fs_data;
  const char *p = path;
  while (*p == '/') p++;

  memset(st, 0, 128);
  uint64_t dev = 5; /* sysfs device */
  uint32_t nlink = 1;
  int32_t blksz = 4096;

  memcpy(st + 0, &dev, 8);
  memcpy(st + 20, &nlink, 4);
  memcpy(st + 56, &blksz, 4);

  /* /sys root */
  if (*p == '\0') {
    uint32_t mode = (uint32_t)(0x4000 | 0555);
    uint64_t ino = 500;
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    return 0;
  }

  if (strcmp(p, "kernel") == 0) {
    uint32_t mode = (uint32_t)(0x4000 | 0555);
    uint64_t ino = 501;
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    return 0;
  }

  if (strcmp(p, "kernel/version") == 0) {
    uint32_t mode = (uint32_t)(0x8000 | 0444);
    uint64_t ino = 502;
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    return 0;
  }

  if (strcmp(p, "kernel/hostname") == 0) {
    uint32_t mode = (uint32_t)(0x8000 | 0444);
    uint64_t ino = 503;
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    return 0;
  }

  if (strcmp(p, "devices") == 0) {
    uint32_t mode = (uint32_t)(0x4000 | 0555);
    uint64_t ino = 510;
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    return 0;
  }

  /* /sys/devices/<name> or /sys/devices/<name>/<attr> */
  if (memcmp(p, "devices/", 8) == 0) {
    const char *rest = p + 8;
    char devname[32];
    int i = 0;
    while (rest[i] && rest[i] != '/' && i < 31) { devname[i] = rest[i]; i++; }
    devname[i] = '\0';

    int dev_idx;
    if (find_device_by_name(devname, &dev_idx) < 0)
      return -2;

    const char *attr = rest + i;
    while (*attr == '/') attr++;

    if (*attr == '\0') {
      /* Device directory */
      uint32_t mode = (uint32_t)(0x4000 | 0555);
      uint64_t ino = (uint64_t)(600 + dev_idx);
      memcpy(st + 8, &ino, 8);
      memcpy(st + 16, &mode, 4);
      return 0;
    }

    /* Device attribute file — validate it exists */
    struct dtb_device *dv = &dtb_devices.devices[dev_idx];
    int attr_id = -1;
    if (strcmp(attr, "address") == 0) attr_id = 0;
    else if (strcmp(attr, "size") == 0) attr_id = 1;
    else if (strcmp(attr, "irq") == 0 && dv->irq > 0) attr_id = 2;
    else if (strcmp(attr, "compatible") == 0 && dv->compatible) attr_id = 3;
    else if (strcmp(attr, "capacity") == 0 &&
             dv->type == DTB_DEV_VIRTIO_MMIO &&
             dev_type_index(dev_idx) == 0) attr_id = 4;
    else if (strcmp(attr, "sector_size") == 0 &&
             dv->type == DTB_DEV_VIRTIO_MMIO &&
             dev_type_index(dev_idx) == 0) attr_id = 5;

    if (attr_id < 0)
      return -2;

    uint32_t mode = (uint32_t)(0x8000 | 0444);
    uint64_t ino = (uint64_t)(700 + dev_idx * 10 + attr_id);
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    return 0;
  }

  return -2; /* -ENOENT */
}

/* ------------------------------------------------------------------ */
/* sysfs_fstat                                                         */
/* ------------------------------------------------------------------ */
static int sysfs_fstat(void *fs_data, void *private, uint8_t *st) {
  (void)fs_data;
  struct sysfs_private *priv = (struct sysfs_private *)private;
  memset(st, 0, 128);
  uint64_t dev = 5;
  uint32_t nlink = 1;
  int32_t blksz = 4096;
  memcpy(st + 0, &dev, 8);
  memcpy(st + 20, &nlink, 4);
  memcpy(st + 56, &blksz, 4);

  if (priv->type == SYSFS_ROOT || priv->type == SYSFS_KERNEL_DIR ||
      priv->type == SYSFS_DEVICES_DIR || priv->type == SYSFS_DEV_DIR) {
    uint32_t mode = (uint32_t)(0x4000 | 0555);
    uint64_t ino;
    switch (priv->type) {
      case SYSFS_ROOT:        ino = 500; break;
      case SYSFS_KERNEL_DIR:  ino = 501; break;
      case SYSFS_DEVICES_DIR: ino = 510; break;
      case SYSFS_DEV_DIR:     ino = (uint64_t)(600 + priv->dev_index); break;
      default:                ino = 500; break;
    }
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
  } else {
    uint32_t mode = (uint32_t)(0x8000 | 0444);
    uint64_t ino = (priv->type == SYSFS_KERNEL_ATTR) ?
      502 : (uint64_t)(700 + priv->dev_index * 10);
    uint64_t size = (uint64_t)priv->len;
    memcpy(st + 8, &ino, 8);
    memcpy(st + 16, &mode, 4);
    memcpy(st + 48, &size, 8);
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* sysfs_readdir                                                       */
/* ------------------------------------------------------------------ */
static int sysfs_readdir(void *fs_data, void *private, char *ubuf,
                         unsigned int count, int64_t *offset) {
  (void)fs_data;
  struct sysfs_private *priv = (struct sysfs_private *)private;
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

  if (priv->type == SYSFS_ROOT) {
    EMIT_DIRENT(".", 500, 4);
    EMIT_DIRENT("..", 1, 4);
    EMIT_DIRENT("kernel", 501, 4);
    EMIT_DIRENT("devices", 510, 4);
  } else if (priv->type == SYSFS_KERNEL_DIR) {
    EMIT_DIRENT(".", 501, 4);
    EMIT_DIRENT("..", 500, 4);
    EMIT_DIRENT("version", 502, 8);
    EMIT_DIRENT("hostname", 503, 8);
  } else if (priv->type == SYSFS_DEVICES_DIR) {
    EMIT_DIRENT(".", 510, 4);
    EMIT_DIRENT("..", 500, 4);
    for (int i = 0; i < dtb_devices.ndevices; i++) {
      const char *tname = dev_type_name(dtb_devices.devices[i].type);
      int tidx = dev_type_index(i);
      char devname[32];
      int pos = 0;
      int tl = strlen(tname);
      memcpy(devname, tname, tl);
      pos += tl;
      /* Append index digit(s) */
      if (tidx == 0) {
        devname[pos++] = '0';
      } else {
        char tmp[12]; int ti = 0;
        int v = tidx;
        while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) devname[pos++] = tmp[--ti];
      }
      devname[pos] = '\0';
      EMIT_DIRENT(devname, (uint64_t)(600 + i), 4);
    }
  } else if (priv->type == SYSFS_DEV_DIR) {
    struct dtb_device *dv = &dtb_devices.devices[priv->dev_index];
    EMIT_DIRENT(".", (uint64_t)(600 + priv->dev_index), 4);
    EMIT_DIRENT("..", 510, 4);
    EMIT_DIRENT("address", (uint64_t)(700 + priv->dev_index * 10 + 0), 8);
    EMIT_DIRENT("size", (uint64_t)(700 + priv->dev_index * 10 + 1), 8);
    if (dv->irq > 0)
      EMIT_DIRENT("irq", (uint64_t)(700 + priv->dev_index * 10 + 2), 8);
    if (dv->compatible)
      EMIT_DIRENT("compatible", (uint64_t)(700 + priv->dev_index * 10 + 3), 8);
    if (dv->type == DTB_DEV_VIRTIO_MMIO && dev_type_index(priv->dev_index) == 0) {
      EMIT_DIRENT("capacity", (uint64_t)(700 + priv->dev_index * 10 + 4), 8);
      EMIT_DIRENT("sector_size", (uint64_t)(700 + priv->dev_index * 10 + 5), 8);
    }
  }

  #undef EMIT_DIRENT

done:
  return (int)written;
}

/* ------------------------------------------------------------------ */
/* sysfs stubs                                                         */
/* ------------------------------------------------------------------ */
static int sysfs_mkdir(void *fs_data, const char *path, int mode) {
  (void)fs_data;
  (void)path; (void)mode;
  return -30; /* -EROFS */
}

static int sysfs_unlink(void *fs_data, const char *path) {
  (void)fs_data;
  (void)path;
  return -30; /* -EROFS */
}

/* ------------------------------------------------------------------ */
/* sysfs VFS ops struct                                                */
/* ------------------------------------------------------------------ */
struct vfs_ops sysfs_ops = {
  .open    = sysfs_open,
  .read    = sysfs_read,
  .write   = sysfs_write,
  .ftruncate = NULL,
  .close   = sysfs_close,
  .stat    = sysfs_stat,
  .fstat   = sysfs_fstat,
  .readdir = sysfs_readdir,
  .mkdir   = sysfs_mkdir,
  .unlink  = sysfs_unlink,
  .rmdir   = NULL,
};
