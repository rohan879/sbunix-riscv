#include <vfs.h>
#include <tar.h>
#include <string.h>
#include <printk.h>
#include <kmalloc.h>
#include <pathres.h>  /* RF: canonical to_tarfs_path prototype */

extern char _tarfs_start[];
extern char _tarfs_end[];

/* ------------------------------------------------------------------ */
/* Private data for an open tarfs file                                  */
/* ------------------------------------------------------------------ */
struct tarfs_private {
  struct posix_header *hdr;  /* pointer to the tar header */
  char *data;                /* pointer to file data (after header) */
  uint64_t size;             /* file size in bytes */
  char dirpath[256];         /* for directories: the canonical dir path */
  int is_dir;                /* 1 if this is a directory */
};

static uint64_t tarfs_parse_octal(const char *field, int len) {
  uint64_t value = 0;
  int seen_digit = 0;

  for (int i = 0; i < len; i++) {
    char c = field[i];
    if (c == '\0')
      break;
    if (c == ' ' && !seen_digit)
      continue;
    if (c < '0' || c > '7')
      break;
    seen_digit = 1;
    value = (value << 3) | (uint64_t)(c - '0');
  }

  return seen_digit ? value : 0;
}

static int64_t tarfs_mtime_sec(const struct posix_header *hdr) {
  uint64_t sec = hdr ? tarfs_parse_octal(hdr->mtime, sizeof(hdr->mtime)) : 0;
  if (sec < 1)
    sec = 1;
  return (int64_t)sec;
}

static void tarfs_fill_stat_times(uint8_t *st, const struct posix_header *hdr) {
  /* BF: tarfs is read-only, but build tools still need nonzero mtimes. */
  int64_t sec = tarfs_mtime_sec(hdr);
  int64_t nsec = 0;

  memcpy(st + 72, &sec, 8);   /* st_atim.tv_sec */
  memcpy(st + 80, &nsec, 8);  /* st_atim.tv_nsec */
  memcpy(st + 88, &sec, 8);   /* st_mtim.tv_sec */
  memcpy(st + 96, &nsec, 8);  /* st_mtim.tv_nsec */
  memcpy(st + 104, &sec, 8);  /* st_ctim.tv_sec */
  memcpy(st + 112, &nsec, 8); /* st_ctim.tv_nsec */
}

/* RF: Removed static to_tarfs_path duplicate — canonical extern
 * implementation lives in kernel/pathres.c (declared in pathres.h).
 * All internal callers use the extern 3-param signature. */

/* Check if path is a directory in tarfs */
static int tarfs_is_dir(const char *path) {
  if (path[0] == '/' && path[1] == '\0') return 1;
  if (path[0] == '.' && path[1] == '\0') return 1;

  char tarpath[256];
  /* BF: Pass sizeof to prevent buffer overflow; return 0 (not dir) on truncation */
  if (to_tarfs_path(path, tarpath, sizeof(tarpath)) < 0) return 0;
  int tplen = strlen(tarpath);

  struct posix_header *hdr = (struct posix_header *)_tarfs_start;
  while ((char *)hdr < _tarfs_end) {
    if (hdr->name[0] == '\0') break;
    int nlen = strlen(hdr->name);
    if (nlen >= tplen && memcmp(hdr->name, tarpath, tplen) == 0 &&
        (hdr->name[tplen] == '/' || hdr->name[tplen] == '\0')) {
      return 1;
    }
    unsigned long size = 0;
    for (int i = 0; i < 11; i++)
      size = size * 8 + (hdr->size[i] - '0');
    hdr = (struct posix_header *)((char *)hdr + 512 + ((size + 511) & ~511UL));
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* tarfs_open                                                           */
/* ------------------------------------------------------------------ */
static int tarfs_open(void *fs_data, const char *path, int flags, void **private) {
  (void)fs_data;
  char tarpath[256];
  /* BF: Pass sizeof to prevent buffer overflow; return ENOENT on truncation */
  if (to_tarfs_path(path, tarpath, sizeof(tarpath)) < 0)
    return -2; /* -ENOENT: path too long for tarfs */

  /* Try exact match first */
  struct posix_header *hdr = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
  if (!hdr && tarpath[0] != '\0') {
    /* Append trailing slash and retry (directories stored with /) */
    int tplen = strlen(tarpath);
    if (tplen < 254) {
      tarpath[tplen] = '/';
      tarpath[tplen + 1] = '\0';
      hdr = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
    }
  }

  /* If no tar entry found, check if it's a known directory (e.g. root) */
  if (!hdr && !tarfs_is_dir(path))
    return -2; /* -ENOENT */

  /* BF: The path layer leaves a final symlink unresolved for O_NOFOLLOW.
   * Reject raw symlink opens instead of exposing the tar link metadata as
   * file contents. */
  if (hdr && hdr->typeflag == '2')
    return -40; /* -ELOOP */

  struct tarfs_private *priv = (struct tarfs_private *)kmalloc(sizeof(struct tarfs_private));
  if (!priv)
    return -12; /* -ENOMEM */

  memset(priv, 0, sizeof(struct tarfs_private));

  if (hdr) {
    priv->hdr = hdr;
    priv->data = (char *)hdr + sizeof(struct posix_header);
    priv->size = tar_get_file_size(hdr);
    priv->is_dir = (hdr->typeflag == '5');

    /* Store canonical directory path for readdir */
    if (priv->is_dir) {
      const char *n = hdr->name;
      if (n[0] == '.' && n[1] == '/') n += 2;
      int nl = strlen(n);
      if (nl > 0 && n[nl - 1] == '/') nl--;
      if (nl > 255) nl = 255;
      memcpy(priv->dirpath, n, nl);
      priv->dirpath[nl] = '\0';
    }
  } else {
    /* Virtual directory (root) */
    priv->hdr = NULL;
    priv->data = NULL;
    priv->size = 0;
    priv->is_dir = 1;
    priv->dirpath[0] = '\0'; /* empty means root */
  }

  *private = priv;
  return 0;
}

/* ------------------------------------------------------------------ */
/* tarfs_read                                                           */
/* ------------------------------------------------------------------ */
static int64_t tarfs_read(void *fs_data, void *private, char *buf,
                          uint64_t count, int64_t *offset) {
  (void)fs_data;
  struct tarfs_private *priv = (struct tarfs_private *)private;
  if (!priv) return -1;
  if (priv->is_dir) return -21; /* -EISDIR */
  if (!priv->data)
    return 0;

  uint64_t off = (uint64_t)*offset;
  uint64_t avail = (off < priv->size) ? (priv->size - off) : 0;
  uint64_t n = (count < avail) ? count : avail;
  if (n > 0) {
    memcpy(buf, priv->data + off, n);
    *offset += (int64_t)n;
  }
  return (int64_t)n;
}

/* ------------------------------------------------------------------ */
/* tarfs_write                                                          */
/* ------------------------------------------------------------------ */
static int64_t tarfs_write(void *fs_data, void *private, const char *buf,
                           uint64_t count, int64_t *offset) {
  (void)fs_data;
  (void)private; (void)buf; (void)count; (void)offset;
  return -30; /* -EROFS */
}

/* ------------------------------------------------------------------ */
/* tarfs_close                                                          */
/* ------------------------------------------------------------------ */
static int tarfs_close(void *fs_data, void *private) {
  (void)fs_data;
  if (private)
    kmfree(private, sizeof(struct tarfs_private));
  return 0;
}

static int tarfs_reopen(void *fs_data, void *private, int flags,
                        void **new_private) {
  (void)fs_data;
  if (!private)
    return -2; /* -ENOENT */
  if (flags & 01000)
    return -30; /* -EROFS */
  struct tarfs_private *priv =
    (struct tarfs_private *)kmalloc(sizeof(struct tarfs_private));
  if (!priv)
    return -12; /* -ENOMEM */
  memcpy(priv, private, sizeof(struct tarfs_private));
  *new_private = priv;
  return 0;
}

/* ------------------------------------------------------------------ */
/* tarfs_fstat                                                          */
/* ------------------------------------------------------------------ */
static int tarfs_fstat(void *fs_data, void *private, uint8_t *st) {
  (void)fs_data;
  struct tarfs_private *priv = (struct tarfs_private *)private;
  if (!priv) return -1;

  memset(st, 0, 128);
  uint64_t dev = 1;
  uint64_t ino = priv->hdr
    ? (uint64_t)((unsigned long)priv->hdr / 512) + 2
    : 1;
  /* Parse permission bits from the tar header's octal mode field */
  uint32_t hdr_mode = 0;
  if (priv->hdr) {
    for (int mi = 0; mi < 8 && priv->hdr->mode[mi] >= '0' && priv->hdr->mode[mi] <= '7'; mi++)
      hdr_mode = (hdr_mode << 3) | (uint32_t)(priv->hdr->mode[mi] - '0');
  } else {
    hdr_mode = priv->is_dir ? 0755 : 0644;
  }
  uint32_t mode;
  if (priv->is_dir)
    mode = (uint32_t)(0x4000 | (hdr_mode & 0777)); /* S_IFDIR */
  else if (priv->hdr && priv->hdr->typeflag == '2')
    mode = (uint32_t)(0xA000 | (hdr_mode & 0777)); /* S_IFLNK */
  else
    mode = (uint32_t)(0x8000 | (hdr_mode & 0777)); /* S_IFREG */
  uint32_t nlink = 1;
  int64_t size = (int64_t)priv->size;
  if (priv->hdr && priv->hdr->typeflag == '2')
    size = (int64_t)strlen(priv->hdr->linkname);
  int32_t blksz = 512;
  int64_t blks = ((int64_t)priv->size + 511) / 512;

  memcpy(st + 0, &dev, 8);
  memcpy(st + 8, &ino, 8);
  memcpy(st + 16, &mode, 4);
  memcpy(st + 20, &nlink, 4);
  memcpy(st + 48, &size, 8);
  memcpy(st + 56, &blksz, 4);
  memcpy(st + 64, &blks, 8);
  tarfs_fill_stat_times(st, priv->hdr);
  return 0;
}

/* ------------------------------------------------------------------ */
/* tarfs_stat                                                           */
/* ------------------------------------------------------------------ */
static int tarfs_stat(void *fs_data, const char *path, uint8_t *st) {
  (void)fs_data;
  char tarpath[256];
  /* BF: Pass sizeof to prevent buffer overflow; return ENOENT on truncation */
  if (to_tarfs_path(path, tarpath, sizeof(tarpath)) < 0)
    return -2; /* -ENOENT: path too long for tarfs */

  struct posix_header *hdr = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
  if (!hdr) {
    /* Try with trailing slash */
    int tplen = strlen(tarpath);
    if (tplen > 0 && tplen < 254 && tarpath[tplen - 1] != '/') {
      tarpath[tplen] = '/';
      tarpath[tplen + 1] = '\0';
      hdr = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
    }
  }

  /* Check for root directory */
  if (!hdr) {
    if ((path[0] == '/' && path[1] == '\0') || (path[0] == '.' && path[1] == '\0')) {
      /* Synthesize stat for root directory */
      memset(st, 0, 128);
      uint64_t rdev = 1; /* tarfs device */
      uint64_t rino = 1;
      uint32_t mode = (uint32_t)(0x4000 | 0755); /* S_IFDIR | 0755 */
      uint32_t nlink = 1;
      int32_t blksz = 512;
      memcpy(st + 0, &rdev, 8);
      memcpy(st + 8, &rino, 8);
      memcpy(st + 16, &mode, 4);
      memcpy(st + 20, &nlink, 4);
      memcpy(st + 56, &blksz, 4);
      tarfs_fill_stat_times(st, NULL);
      return 0;
    }
    return -2; /* -ENOENT */
  }

  memset(st, 0, 128);
  uint64_t tdev = 1; /* tarfs device */
  /* Use header address as a unique inode number */
  uint64_t tino = (uint64_t)((unsigned long)hdr / 512) + 2;
  uint64_t fsz = tar_get_file_size(hdr);
  /* Parse permission bits from the tar header's octal mode field */
  uint32_t hdr_mode = 0;
  for (int mi = 0; mi < 8 && hdr->mode[mi] >= '0' && hdr->mode[mi] <= '7'; mi++)
    hdr_mode = (hdr_mode << 3) | (uint32_t)(hdr->mode[mi] - '0');
  uint32_t mode;
  if (hdr->typeflag == '5')
    mode = (uint32_t)(0x4000 | (hdr_mode & 0777)); /* S_IFDIR */
  else if (hdr->typeflag == '2')
    mode = (uint32_t)(0xA000 | (hdr_mode & 0777)); /* S_IFLNK */
  else
    mode = (uint32_t)(0x8000 | (hdr_mode & 0777)); /* S_IFREG */
  uint32_t nlink = 1;
  int64_t size = (hdr->typeflag == '2')
    ? (int64_t)strlen(hdr->linkname)
    : (int64_t)fsz;
  int32_t blksz = 512;
  int64_t blks = ((int64_t)fsz + 511) / 512;
  memcpy(st + 0, &tdev, 8);
  memcpy(st + 8, &tino, 8);
  memcpy(st + 16, &mode, 4);
  memcpy(st + 20, &nlink, 4);
  memcpy(st + 48, &size, 8);
  memcpy(st + 56, &blksz, 4);
  memcpy(st + 64, &blks, 8);
  tarfs_fill_stat_times(st, hdr);
  return 0;
}

/* ------------------------------------------------------------------ */
/* tarfs_readdir                                                        */
/* ------------------------------------------------------------------ */
static int tarfs_readdir(void *fs_data, void *private, char *ubuf,
                         unsigned int count, int64_t *offset) {
  (void)fs_data;
  struct tarfs_private *priv = (struct tarfs_private *)private;
  if (!priv || !priv->is_dir)
    return -1;

  int dirpath_len = strlen(priv->dirpath);
  uint64_t skip = (uint64_t)*offset;
  unsigned int written = 0;
  uint64_t seen = 0;

  /* Helper macro: emit one dirent into ubuf */
#define EMIT_DIRENT(name_, ino_, dtype_) do { \
    int _nl = strlen(name_); \
    int _rl = (19 + _nl + 1 + 7) & ~7; \
    if (written + (unsigned int)_rl > count) goto done; \
    char *_d = ubuf + written; \
    memset(_d, 0, _rl); \
    uint64_t _ino = (uint64_t)(ino_); \
    int64_t _off_val = (int64_t)(seen + 1); \
    uint16_t _rlen = (uint16_t)_rl; \
    uint8_t _dt = (uint8_t)(dtype_); \
    memcpy(_d + 0, &_ino, 8); \
    memcpy(_d + 8, &_off_val, 8); \
    memcpy(_d + 16, &_rlen, 2); \
    memcpy(_d + 18, &_dt, 1); \
    memcpy(_d + 19, (name_), _nl + 1); \
    written += (unsigned int)_rl; \
    *offset = (int64_t)(seen + 1); \
  } while (0)

  /* Synthetic "." entry */
  if (seen >= skip) {
    uint64_t dot_ino = priv->hdr
      ? (uint64_t)((char *)priv->hdr - _tarfs_start) / 512 + 1
      : 1;
    EMIT_DIRENT(".", dot_ino, 4 /* DT_DIR */);
  }
  seen++;

  /* Synthetic ".." entry */
  if (seen >= skip) {
    EMIT_DIRENT("..", 1, 4 /* DT_DIR */);
  }
  seen++;

  /* Scan tar entries */
  struct posix_header *hdr = (struct posix_header *)_tarfs_start;
  while ((char *)hdr < _tarfs_end && written + 32 <= count) {
    if (hdr->name[0] == '\0') break;
    if (strncmp(hdr->magic, "ustar", 5) != 0) {
      hdr = (struct posix_header *)((char *)hdr + 512);
      continue;
    }

    /* Strip "./" prefix from tar name */
    const char *ename = hdr->name;
    if (ename[0] == '.' && ename[1] == '/') ename += 2;

    /* Remove trailing slash for comparison */
    char clean[100];
    int elen = strlen(ename);
    if (elen > 0 && ename[elen - 1] == '/') elen--;
    if (elen > 99) elen = 99;
    memcpy(clean, ename, elen);
    clean[elen] = '\0';

    /* Check if this entry's parent is dirpath */
    int is_child = 0;
    if (dirpath_len == 0) {
      /* Root directory: direct children have no '/' in clean */
      is_child = (elen > 0 && strchr(clean, '/') == NULL);
    } else {
      if (elen > dirpath_len + 1 &&
          memcmp(clean, priv->dirpath, dirpath_len) == 0 &&
          clean[dirpath_len] == '/') {
        const char *rest = clean + dirpath_len + 1;
        is_child = (strchr(rest, '/') == NULL && rest[0] != '\0');
      }
    }

    if (is_child) {
      if (seen >= skip) {
        const char *basename = clean + (dirpath_len > 0 ? dirpath_len + 1 : 0);
        int namelen = strlen(basename);
        int reclen = (19 + namelen + 1 + 7) & ~7;
        if (written + (unsigned int)reclen > count) break;

        char *d = ubuf + written;
        memset(d, 0, reclen);
        uint64_t ino = (uint64_t)((char *)hdr - _tarfs_start) / 512 + 1;
        int64_t doff = (int64_t)(seen + 1);
        uint16_t rlen = (uint16_t)reclen;
        uint8_t dtype = (hdr->typeflag == '5') ? 4 :
                        (hdr->typeflag == '2') ? 10 : 8;
        memcpy(d + 0, &ino, 8);
        memcpy(d + 8, &doff, 8);
        memcpy(d + 16, &rlen, 2);
        memcpy(d + 18, &dtype, 1);
        memcpy(d + 19, basename, namelen + 1);
        written += (unsigned int)reclen;
        *offset = (int64_t)(seen + 1);
      }
      seen++;
    }

    /* Advance to next tar entry */
    uint64_t fsz2 = tar_get_file_size(hdr);
    hdr = (struct posix_header *)((char *)hdr + 512 + ((fsz2 + 511) & ~511UL));
  }

#undef EMIT_DIRENT

done:
  return (int)written;
}

/* ------------------------------------------------------------------ */
/* tarfs_readlink                                                       */
/* ------------------------------------------------------------------ */
static int tarfs_readlink(void *fs_data, const char *path, char *buf, int bufsiz) {
  (void)fs_data;
  char tarpath[256];
  /* BF: Pass sizeof to prevent buffer overflow; return ENOENT on truncation */
  if (to_tarfs_path(path, tarpath, sizeof(tarpath)) < 0)
    return -2; /* -ENOENT: path too long for tarfs */

  struct posix_header *hdr = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
  if (!hdr) {
    int tplen = strlen(tarpath);
    if (tplen > 0 && tplen < 254 && tarpath[tplen - 1] != '/') {
      tarpath[tplen] = '/';
      tarpath[tplen + 1] = '\0';
      hdr = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
    }
  }
  if (!hdr && tarfs_is_dir(path))
    return -22; /* EINVAL — directories are not symlinks */
  if (!hdr)
    return -2; /* ENOENT */

  if (hdr->typeflag != '2')
    return -22; /* EINVAL — not a symlink */

  int len = strlen(hdr->linkname);
  if (len > bufsiz)
    len = bufsiz;
  memcpy(buf, hdr->linkname, len);
  return len;
}

/* ------------------------------------------------------------------ */
/* tarfs_mkdir / tarfs_unlink                                           */
/* ------------------------------------------------------------------ */
static int tarfs_mkdir(void *fs_data, const char *path, int mode) {
  (void)fs_data;
  (void)path; (void)mode;
  return -30; /* -EROFS */
}

static int tarfs_unlink(void *fs_data, const char *path) {
  (void)fs_data;
  (void)path;
  return -30; /* -EROFS */
}

/* ------------------------------------------------------------------ */
/* tarfs VFS ops struct                                                 */
/* ------------------------------------------------------------------ */
struct vfs_ops tarfs_ops = {
  .open    = tarfs_open,
  .read    = tarfs_read,
  .write   = tarfs_write,
  .ftruncate = NULL,
  .reopen  = tarfs_reopen,
  .close   = tarfs_close,
  .stat    = tarfs_stat,
  .fstat   = tarfs_fstat,
  .readdir = tarfs_readdir,
  .mkdir   = tarfs_mkdir,
  .unlink  = tarfs_unlink,
  .rmdir   = NULL,
  .readlink = tarfs_readlink,
};
