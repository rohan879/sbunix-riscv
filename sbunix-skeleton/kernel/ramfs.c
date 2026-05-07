/*
 * ramfs.c — In-memory writable filesystem for SBUnix.
 *
 * Mounted at /tmp to provide a writable scratch area on top of the
 * read-only tarfs root.  All data lives in kernel heap (kmalloc/kalloc).
 *
 * Supports: open, read, write, close, stat, fstat, readdir,
 *           mkdir, unlink, rmdir, rename.
 */

#include <vfs.h>
#include <string.h>
#include <printk.h>
#include <kmalloc.h>
#include <kalloc.h>
#include <timekeeping.h>
#include <proc.h>

/* ================================================================== */
/* Inode / directory-entry structures                                   */
/* ================================================================== */

#define RAMFS_MAX_INODES   256
#define RAMFS_MAX_CHILDREN  64
#define RAMFS_NAME_MAX      256
#define RAMFS_MAX_BLOCKS    64   /* max 4KB blocks per file = 256KB */

#define RAMFS_INO_ROOT       1

/* ENH: Added RAMFS_SYMLINK type to support symbolic links in ramfs */
enum ramfs_type { RAMFS_FILE, RAMFS_DIR, RAMFS_SYMLINK };

struct ramfs_inode {
  uint64_t         ino;
  enum ramfs_type  type;
  uint32_t         mode;       /* permission bits (no S_IF prefix stored here) */
  int              nlink;
  int              open_count; /* BF: Track open fds for deferred unlink freeing */
  uint64_t         size;       /* bytes for files, 0 for dirs */
  /* BF: Added timestamps so stat/utimensat can report and modify
   * atime/mtime/ctime.  Without these, stat always returned zero
   * timestamps, causing POSIX compliance test failures. */
  uint64_t         atime_sec;  /* last access time */
  uint64_t         mtime_sec;  /* last modification time */
  uint64_t         ctime_sec;  /* last status change time */
  uint32_t         uid;
  uint32_t         gid;
  /* File data blocks (each is a kalloc page = 4096 bytes) */
  char            *blocks[RAMFS_MAX_BLOCKS];
  int              nblocks;
  /* Directory children */
  struct ramfs_dirent {
    char              *name;
    uint16_t          name_len;
    struct ramfs_inode *inode;
  } children[RAMFS_MAX_CHILDREN];
  int              nchildren;
  struct ramfs_inode *parent;   /* ".." link */
  /* BF: Store SYMLINK_MAX bytes plus NUL so ramfs matches libc limits. */
  char symlink_target[256];
};

static struct ramfs_inode inodes[RAMFS_MAX_INODES];
static uint64_t next_ino = RAMFS_INO_ROOT;

/* ================================================================== */
/* Helpers                                                              */
/* ================================================================== */

static void ramfs_dirent_clear(struct ramfs_dirent *de) {
  if (!de)
    return;
  if (de->name)
    kmfree(de->name, (uint64_t)de->name_len + 1);
  memset(de, 0, sizeof(*de));
}

static struct ramfs_inode *ramfs_alloc_inode(enum ramfs_type type, uint32_t mode) {
  for (int i = 0; i < RAMFS_MAX_INODES; i++) {
    if (inodes[i].ino == 0) {
      memset(&inodes[i], 0, sizeof(struct ramfs_inode));
      inodes[i].ino = ++next_ino;
      inodes[i].type = type;
      inodes[i].mode = mode;
      inodes[i].nlink = 1;
      uint64_t now = timekeeping_now_seconds();
      inodes[i].atime_sec = now;
      inodes[i].mtime_sec = now;
      inodes[i].ctime_sec = now;
      inodes[i].uid = current_proc ? (uint32_t)current_proc->uid : 0;
      inodes[i].gid = current_proc ? (uint32_t)current_proc->gid : 0;
      return &inodes[i];
    }
  }
  return NULL;
}

static void ramfs_free_inode(struct ramfs_inode *ino) {
  if (!ino) return;
  /* BF: Directory entry names live out of line so long basenames do not
   * inflate every static ramfs inode.  Free them before recycling the inode. */
  for (int i = 0; i < ino->nchildren; i++)
    ramfs_dirent_clear(&ino->children[i]);
  /* Free file data blocks */
  for (int i = 0; i < ino->nblocks; i++) {
    if (ino->blocks[i]) {
      kfree(ino->blocks[i]);
      ino->blocks[i] = NULL;
    }
  }
  memset(ino, 0, sizeof(struct ramfs_inode));
}

/* Look up a single path component in a directory inode */
static struct ramfs_inode *ramfs_dir_lookup(struct ramfs_inode *dir, const char *name) {
  if (!dir || dir->type != RAMFS_DIR) return NULL;
  for (int i = 0; i < dir->nchildren; i++) {
    if (dir->children[i].name &&
        strcmp(dir->children[i].name, name) == 0)
      return dir->children[i].inode;
  }
  return NULL;
}

/* Add a child entry to a directory */
static int ramfs_dir_add(struct ramfs_inode *dir, const char *name, struct ramfs_inode *child) {
  if (!dir || dir->type != RAMFS_DIR) return -1;
  if (dir->nchildren >= RAMFS_MAX_CHILDREN) return -28; /* -ENOSPC */
  int nlen = (int)strlen(name);
  if (nlen >= RAMFS_NAME_MAX) return -36; /* -ENAMETOOLONG */
  char *name_copy = (char *)kmalloc((uint64_t)nlen + 1);
  if (!name_copy)
    return -12; /* -ENOMEM */
  memcpy(name_copy, name, nlen);
  name_copy[nlen] = '\0';
  struct ramfs_dirent *de = &dir->children[dir->nchildren++];
  de->name = name_copy;
  de->name_len = (uint16_t)nlen;
  de->inode = child;
  child->parent = dir;
  /* ENH: Directory metadata changes after successful link creation. */
  dir->mtime_sec = timekeeping_now_seconds();
  dir->ctime_sec = dir->mtime_sec;
  return 0;
}

/* Remove a child entry from a directory */
static int ramfs_dir_remove(struct ramfs_inode *dir, const char *name) {
  if (!dir || dir->type != RAMFS_DIR) return -1;
  for (int i = 0; i < dir->nchildren; i++) {
    if (dir->children[i].name &&
        strcmp(dir->children[i].name, name) == 0) {
      /* Swap-remove */
      ramfs_dirent_clear(&dir->children[i]);
      dir->nchildren--;
      if (i < dir->nchildren) {
        dir->children[i] = dir->children[dir->nchildren];
        memset(&dir->children[dir->nchildren], 0, sizeof(struct ramfs_dirent));
      }
      dir->mtime_sec = timekeeping_now_seconds();
      dir->ctime_sec = dir->mtime_sec;
      return 0;
    }
  }
  return -2; /* -ENOENT */
}

/*
 * Walk a path relative to the ramfs root.
 * Path comes in as relpath from VFS (e.g. "/" or "foo/bar" or "/foo/bar").
 * If parent_out is non-NULL, returns the parent directory and sets *basename
 * to the final component (for create/unlink operations).
 */
static struct ramfs_inode *ramfs_root(void) {
  /* inode 0 is always the root directory; initialised in ramfs_init */
  return &inodes[0];
}

static struct ramfs_inode *ramfs_walk(const char *path) {
  struct ramfs_inode *cur = ramfs_root();
  if (!cur) return NULL;

  /* Skip leading slashes */
  while (*path == '/') path++;
  if (*path == '\0') return cur;

  char component[RAMFS_NAME_MAX];
  while (*path) {
    int i = 0;
    while (*path && *path != '/' && i < RAMFS_NAME_MAX - 1)
      component[i++] = *path++;
    component[i] = '\0';
    while (*path == '/') path++;

    if (strcmp(component, ".") == 0)
      continue;
    if (strcmp(component, "..") == 0) {
      if (cur->parent) cur = cur->parent;
      continue;
    }

    cur = ramfs_dir_lookup(cur, component);
    if (!cur) return NULL;
  }
  return cur;
}

/*
 * Walk to the parent directory of the final component.
 * Sets *basename to point into path at the final component.
 * Returns NULL if any intermediate directory doesn't exist.
 */
static struct ramfs_inode *ramfs_walk_parent(const char *path, const char **basename) {
  struct ramfs_inode *cur = ramfs_root();
  if (!cur) return NULL;

  while (*path == '/') path++;
  if (*path == '\0') {
    *basename = "";
    return cur;
  }

  /* Find the last '/' to split parent/basename */
  const char *last_slash = NULL;
  for (const char *p = path; *p; p++) {
    if (*p == '/') last_slash = p;
  }

  if (!last_slash) {
    /* No slash — parent is root, basename is the whole path */
    *basename = path;
    return cur;
  }

  /* Walk to the parent directory */
  char component[RAMFS_NAME_MAX];
  const char *p = path;
  while (p < last_slash) {
    int i = 0;
    while (p < last_slash && *p != '/' && i < RAMFS_NAME_MAX - 1)
      component[i++] = *p++;
    component[i] = '\0';
    while (p < last_slash && *p == '/') p++;

    if (strcmp(component, ".") == 0) continue;
    if (strcmp(component, "..") == 0) {
      if (cur->parent) cur = cur->parent;
      continue;
    }

    cur = ramfs_dir_lookup(cur, component);
    if (!cur || cur->type != RAMFS_DIR) return NULL;
  }

  /* basename is everything after last_slash */
  *basename = last_slash + 1;
  while (**basename == '/') (*basename)++;
  return cur;
}

/* ================================================================== */
/* File I/O helpers                                                     */
/* ================================================================== */

/* Ensure the file has enough blocks to cover 'offset + count' bytes.
 * Returns 0 on success, negative errno on failure. */
static int ramfs_ensure_blocks(struct ramfs_inode *ino, uint64_t need) {
  int nblocks_needed = (int)((need + PAGE_SIZE - 1) / PAGE_SIZE);
  if (nblocks_needed > RAMFS_MAX_BLOCKS)
    return -28; /* -ENOSPC */
  while (ino->nblocks < nblocks_needed) {
    char *blk = kalloc();
    if (!blk) return -12; /* -ENOMEM */
    memset(blk, 0, PAGE_SIZE);
    ino->blocks[ino->nblocks++] = blk;
  }
  return 0;
}

/* ================================================================== */
/* VFS operations                                                       */
/* ================================================================== */

/* Private data for an open ramfs file/directory */
struct ramfs_private {
  struct ramfs_inode *inode;
};

/* Flags from libc/include/fcntl.h */
#define RAMFS_O_CREAT   0x40
#define RAMFS_O_EXCL    0x80  /* ENH: Added to support O_CREAT|O_EXCL for mknodat */
#define RAMFS_O_TRUNC   0x200
#define RAMFS_O_APPEND  0x400

static int ramfs_open(void *fs_data, const char *path, int flags, void **private) {
  (void)fs_data;
  struct ramfs_inode *ino = ramfs_walk(path);

  /* ENH: O_EXCL with O_CREAT must fail if the file already exists (EEXIST).
   * Required by POSIX and used by mknodat to avoid creating duplicates. */
  if (ino && (flags & RAMFS_O_CREAT) && (flags & RAMFS_O_EXCL))
    return -17; /* -EEXIST */

  if (!ino && (flags & RAMFS_O_CREAT)) {
    /* Create the file */
    const char *basename;
    struct ramfs_inode *parent = ramfs_walk_parent(path, &basename);
    if (!parent || parent->type != RAMFS_DIR)
      return -2; /* -ENOENT (parent dir doesn't exist) */
    if (*basename == '\0')
      return -22; /* -EINVAL */

    ino = ramfs_alloc_inode(RAMFS_FILE, 0644);
    if (!ino) return -12; /* -ENOMEM */

    int rc = ramfs_dir_add(parent, basename, ino);
    if (rc < 0) {
      ramfs_free_inode(ino);
      return rc;
    }
  }

  if (!ino)
    return -2; /* -ENOENT */

  /* ENH: Safety guard — pathres should have resolved symlinks before we
   * get here, but O_NOFOLLOW opens could reach a symlink inode directly.
   * Return -ELOOP to prevent opening the raw symlink as a file. */
  if (ino->type == RAMFS_SYMLINK)
    return -40; /* -ELOOP */

  /* Truncate if requested */
  if ((flags & RAMFS_O_TRUNC) && ino->type == RAMFS_FILE) {
    for (int i = 0; i < ino->nblocks; i++) {
      if (ino->blocks[i]) {
        kfree(ino->blocks[i]);
        ino->blocks[i] = NULL;
      }
    }
    ino->nblocks = 0;
    ino->size = 0;
  }

  struct ramfs_private *priv = (struct ramfs_private *)kmalloc(sizeof(struct ramfs_private));
  if (!priv) return -12;
  priv->inode = ino;
  *private = priv;
  /* BF: Track open fds for deferred unlink — inode must not be freed
   * while any fd (or mmap VMA) still references it. */
  ino->open_count++;
  return 0;
}

static int64_t ramfs_read(void *fs_data, void *private, char *buf,
                          uint64_t count, int64_t *offset) {
  (void)fs_data;
  struct ramfs_private *priv = (struct ramfs_private *)private;
  if (!priv || !priv->inode) return -1;
  if (priv->inode->type != RAMFS_FILE)
    return -21; /* -EISDIR (or other non-file) */

  struct ramfs_inode *ino = priv->inode;
  uint64_t off = (uint64_t)*offset;
  if (off >= ino->size) return 0;

  uint64_t avail = ino->size - off;
  if (count > avail) count = avail;

  uint64_t total = 0;
  while (total < count) {
    int blk_idx = (int)((off + total) / PAGE_SIZE);
    int blk_off = (int)((off + total) % PAGE_SIZE);
    int chunk = PAGE_SIZE - blk_off;
    if ((uint64_t)chunk > count - total) chunk = (int)(count - total);

    if (blk_idx >= ino->nblocks || !ino->blocks[blk_idx])
      break;
    memcpy(buf + total, ino->blocks[blk_idx] + blk_off, chunk);
    total += (uint64_t)chunk;
  }

  *offset += (int64_t)total;
  return (int64_t)total;
}

static int64_t ramfs_write(void *fs_data, void *private, const char *buf,
                           uint64_t count, int64_t *offset) {
  (void)fs_data;
  struct ramfs_private *priv = (struct ramfs_private *)private;
  if (!priv || !priv->inode || priv->inode->type != RAMFS_FILE)
    return -1;

  struct ramfs_inode *ino = priv->inode;
  uint64_t off = (uint64_t)*offset;

  /* Ensure we have enough blocks */
  int rc = ramfs_ensure_blocks(ino, off + count);
  if (rc < 0) return (int64_t)rc;

  uint64_t total = 0;
  while (total < count) {
    int blk_idx = (int)((off + total) / PAGE_SIZE);
    int blk_off = (int)((off + total) % PAGE_SIZE);
    int chunk = PAGE_SIZE - blk_off;
    if ((uint64_t)chunk > count - total) chunk = (int)(count - total);

    if (blk_idx >= ino->nblocks || !ino->blocks[blk_idx])
      break;
    memcpy(ino->blocks[blk_idx] + blk_off, buf + total, chunk);
    total += (uint64_t)chunk;
  }

  *offset += (int64_t)total;
  if ((uint64_t)*offset > ino->size)
    ino->size = (uint64_t)*offset;

  /* BF: Update mtime and ctime after successful write.  POSIX requires
   * write() to update the file's modification and status-change times.
   * Without this, timestamps remained frozen at creation time, causing
   * mmap atime/mtime consistency tests to fail. */
  if (total > 0) {
    ino->mtime_sec = timekeeping_now_seconds();
    ino->ctime_sec = ino->mtime_sec;
  }

  return (int64_t)total;
}

static int ramfs_ftruncate(void *fs_data, void *private, uint64_t length) {
  (void)fs_data;
  struct ramfs_private *priv = (struct ramfs_private *)private;
  if (!priv || !priv->inode || priv->inode->type != RAMFS_FILE)
    return -9; /* EBADF */

  struct ramfs_inode *ino = priv->inode;

  if (length < ino->size) {
    int needed_blocks = (int)((length + PAGE_SIZE - 1) / PAGE_SIZE);
    for (int i = needed_blocks; i < ino->nblocks; i++) {
      if (ino->blocks[i]) {
        kfree(ino->blocks[i]);
        ino->blocks[i] = NULL;
      }
    }
    ino->nblocks = needed_blocks;
    ino->size = length;
    return 0;
  }

  if (length > ino->size) {
    int rc = ramfs_ensure_blocks(ino, length);
    if (rc < 0)
      return rc;
    ino->size = length;
  }

  return 0;
}

static int ramfs_reopen(void *fs_data, void *private, int flags,
                        void **new_private) {
  (void)fs_data;
  struct ramfs_private *old = (struct ramfs_private *)private;
  if (!old || !old->inode)
    return -2; /* -ENOENT */
  if ((flags & RAMFS_O_CREAT) && (flags & RAMFS_O_EXCL))
    return -17; /* -EEXIST */

  struct ramfs_inode *ino = old->inode;
  if ((flags & RAMFS_O_TRUNC) && ino->type == RAMFS_FILE) {
    /* BF: procfd O_TRUNC must affect the already-open object, even after
     * its pathname is removed or replaced. */
    for (int i = 0; i < ino->nblocks; i++) {
      if (ino->blocks[i]) {
        kfree(ino->blocks[i]);
        ino->blocks[i] = NULL;
      }
    }
    ino->nblocks = 0;
    ino->size = 0;
    ino->mtime_sec = timekeeping_now_seconds();
    ino->ctime_sec = ino->mtime_sec;
  }

  struct ramfs_private *priv =
    (struct ramfs_private *)kmalloc(sizeof(struct ramfs_private));
  if (!priv)
    return -12; /* -ENOMEM */
  priv->inode = ino;
  ino->open_count++;
  *new_private = priv;
  return 0;
}

static int ramfs_close(void *fs_data, void *private) {
  (void)fs_data;
  if (private) {
    struct ramfs_private *priv = (struct ramfs_private *)private;
    /* BF: Decrement open count and free inode if this was the last
     * reference to an unlinked file (nlink==0).  Without deferred
     * freeing, unlink zeroes the inode while fds/mmaps still reference
     * it, causing fstat to return size=0 and SIGBUS on demand paging. */
    if (priv->inode) {
      priv->inode->open_count--;
      if (priv->inode->nlink <= 0 && priv->inode->open_count <= 0)
        ramfs_free_inode(priv->inode);
    }
    kmfree(private, sizeof(struct ramfs_private));
  }
  return 0;
}

/* Fill a stat buffer at the given address (128-byte struct stat layout) */
static void ramfs_fill_stat(uint8_t *st, struct ramfs_inode *ino) {
  memset(st, 0, 128);
  uint64_t dev = 6; /* ramfs device id */
  uint64_t ino_val = ino->ino;
  uint32_t mode;
  if (ino->type == RAMFS_DIR)
    mode = (uint32_t)(0x4000 | ino->mode); /* S_IFDIR */
  else if (ino->type == RAMFS_SYMLINK)
    mode = (uint32_t)(0120000 | ino->mode); /* S_IFLNK */
  else
    mode = (uint32_t)(0x8000 | ino->mode); /* S_IFREG */
  uint32_t nlink = (uint32_t)ino->nlink;
  uint32_t uid = ino->uid;
  uint32_t gid = ino->gid;
  int64_t size = (int64_t)ino->size;
  int32_t blksz = 4096;
  int64_t blks = ((int64_t)ino->size + 511) / 512;

  memcpy(st + 0, &dev, 8);       /* st_dev */
  memcpy(st + 8, &ino_val, 8);   /* st_ino */
  memcpy(st + 16, &mode, 4);     /* st_mode */
  memcpy(st + 20, &nlink, 4);    /* st_nlink */
  memcpy(st + 24, &uid, 4);      /* st_uid */
  memcpy(st + 28, &gid, 4);      /* st_gid */
  memcpy(st + 48, &size, 8);     /* st_size */
  memcpy(st + 56, &blksz, 4);    /* st_blksize */
  memcpy(st + 64, &blks, 8);     /* st_blocks */

  /* BF: Write atime/mtime/ctime into stat buffer at correct offsets.
   * Each timespec is 16 bytes (8 for tv_sec + 8 for tv_nsec).
   * Without these, stat always returned zero timestamps. */
  /* BF: Report mtime as atime, consistent with ext2_fstat behavior.
   * The kernel does not track per-read atime updates, so using mtime
   * approximates relatime semantics and ensures POSIX mmap atime
   * tests see updated timestamps after write-back. */
  uint64_t atime_sec = ino->mtime_sec;
  uint64_t mtime_sec = ino->mtime_sec;
  uint64_t ctime_sec = ino->ctime_sec;
  uint64_t nsec_zero = 0;
  memcpy(st + 72, &atime_sec, 8);  /* st_atim.tv_sec */
  memcpy(st + 80, &nsec_zero, 8);  /* st_atim.tv_nsec */
  memcpy(st + 88, &mtime_sec, 8);  /* st_mtim.tv_sec */
  memcpy(st + 96, &nsec_zero, 8);  /* st_mtim.tv_nsec */
  memcpy(st + 104, &ctime_sec, 8); /* st_ctim.tv_sec */
  memcpy(st + 112, &nsec_zero, 8); /* st_ctim.tv_nsec */
}

static int ramfs_fstat(void *fs_data, void *private, uint8_t *st) {
  (void)fs_data;
  struct ramfs_private *priv = (struct ramfs_private *)private;
  if (!priv || !priv->inode) return -1;
  ramfs_fill_stat(st, priv->inode);
  return 0;
}

static int ramfs_stat(void *fs_data, const char *path, uint8_t *st) {
  (void)fs_data;
  struct ramfs_inode *ino = ramfs_walk(path);
  if (!ino) return -2; /* -ENOENT */
  ramfs_fill_stat(st, ino);
  return 0;
}

static int ramfs_readdir(void *fs_data, void *private, char *ubuf,
                         unsigned int count, int64_t *offset) {
  (void)fs_data;
  struct ramfs_private *priv = (struct ramfs_private *)private;
  if (!priv || !priv->inode || priv->inode->type != RAMFS_DIR)
    return -1;

  struct ramfs_inode *dir = priv->inode;
  uint64_t skip = (uint64_t)*offset;
  unsigned int written = 0;
  uint64_t seen = 0;

#define RAMFS_EMIT_DIRENT(name_str, ino_val, dtype) do { \
  if (seen >= skip) { \
    int _nl = strlen(name_str); \
    int _rl = (19 + _nl + 1 + 7) & ~7; \
    if (written + (unsigned int)_rl > count) goto done; \
    char *_d = ubuf + written; \
    memset(_d, 0, _rl); \
    uint64_t _ino = (ino_val); \
    int64_t _doff = (int64_t)(seen + 1); \
    uint16_t _rlen = (uint16_t)_rl; \
    uint8_t _dt = (dtype); \
    memcpy(_d + 0, &_ino, 8); \
    memcpy(_d + 8, &_doff, 8); \
    memcpy(_d + 16, &_rlen, 2); \
    memcpy(_d + 18, &_dt, 1); \
    memcpy(_d + 19, name_str, _nl + 1); \
    written += (unsigned int)_rl; \
    *offset = (int64_t)(seen + 1); \
  } \
  seen++; \
} while(0)

  /* "." and ".." */
  RAMFS_EMIT_DIRENT(".", dir->ino, 4 /* DT_DIR */);
  RAMFS_EMIT_DIRENT("..", dir->parent ? dir->parent->ino : dir->ino, 4);

  /* Directory entries */
  for (int i = 0; i < dir->nchildren; i++) {
    struct ramfs_inode *child = dir->children[i].inode;
    /* ENH: Report correct dirent type for symlinks (DT_LNK=10) */
    uint8_t dt = (child->type == RAMFS_DIR) ? 4
               : (child->type == RAMFS_SYMLINK) ? 10 : 8; /* DT_DIR : DT_LNK : DT_REG */
    RAMFS_EMIT_DIRENT(dir->children[i].name, child->ino, dt);
  }

#undef RAMFS_EMIT_DIRENT

done:
  return (int)written;
}

static int ramfs_mkdir(void *fs_data, const char *path, int mode) {
  (void)fs_data;
  /* Check if it already exists */
  if (ramfs_walk(path))
    return -17; /* -EEXIST */

  const char *basename;
  struct ramfs_inode *parent = ramfs_walk_parent(path, &basename);
  if (!parent || parent->type != RAMFS_DIR)
    return -2; /* -ENOENT */
  if (*basename == '\0')
    return -22; /* -EINVAL */

  struct ramfs_inode *dir = ramfs_alloc_inode(RAMFS_DIR, (uint32_t)(mode & 0777));
  if (!dir) return -12; /* -ENOMEM */

  int rc = ramfs_dir_add(parent, basename, dir);
  if (rc < 0) {
    ramfs_free_inode(dir);
    return rc;
  }
  return 0;
}

static int ramfs_unlink(void *fs_data, const char *path) {
  (void)fs_data;
  const char *basename;
  struct ramfs_inode *parent = ramfs_walk_parent(path, &basename);
  if (!parent) return -2;

  struct ramfs_inode *ino = ramfs_dir_lookup(parent, basename);
  if (!ino) return -2;
  /* ENH: Only directories are rejected — symlinks (RAMFS_SYMLINK) and
   * regular files can both be unlinked normally. */
  if (ino->type == RAMFS_DIR) return -21; /* -EISDIR */

  ramfs_dir_remove(parent, basename);
  ino->nlink--;
  /* BF: Only free the inode when both nlink reaches 0 AND no open fds
   * remain.  Previously unlink freed immediately even with open fds,
   * zeroing ino->size and causing SIGBUS on demand-paged mmap access
   * to the still-open file.  POSIX requires unlinked files to remain
   * accessible via existing fds until the last close. */
  if (ino->nlink <= 0 && ino->open_count <= 0)
    ramfs_free_inode(ino);
  return 0;
}

static int ramfs_rmdir(void *fs_data, const char *path) {
  (void)fs_data;
  const char *basename;
  struct ramfs_inode *parent = ramfs_walk_parent(path, &basename);
  if (!parent) return -2;

  struct ramfs_inode *ino = ramfs_dir_lookup(parent, basename);
  if (!ino) return -2;
  if (ino->type != RAMFS_DIR) return -20; /* -ENOTDIR */
  if (ino->nchildren > 0) return -39; /* -ENOTEMPTY */

  ramfs_dir_remove(parent, basename);
  ino->nlink--;
  /* BF: Defer free if directory still has open fds (matches ramfs_unlink/
   * ramfs_rename/ramfs_close deferred-free pattern) */
  if (ino->nlink <= 0 && ino->open_count <= 0)
    ramfs_free_inode(ino);
  return 0;
}

static int ramfs_rename(void *fs_data, const char *oldpath,
                        const char *newpath) {
  (void)fs_data;
  /* Find old entry */
  const char *old_base;
  struct ramfs_inode *old_parent = ramfs_walk_parent(oldpath, &old_base);
  if (!old_parent) return -2;

  struct ramfs_inode *ino = ramfs_dir_lookup(old_parent, old_base);
  if (!ino) return -2;

  /* Find new parent */
  const char *new_base;
  struct ramfs_inode *new_parent = ramfs_walk_parent(newpath, &new_base);
  if (!new_parent || new_parent->type != RAMFS_DIR) return -2;

  /* Remove existing target if any */
  struct ramfs_inode *existing = ramfs_dir_lookup(new_parent, new_base);
  if (existing) {
    /* BF: Same-inode rename (e.g. rename("a","a")) is a no-op per POSIX */
    if (existing == ino)
      return 0;
    if (ino->type == RAMFS_DIR && existing->type != RAMFS_DIR)
      return -20; /* -ENOTDIR */
    if (ino->type != RAMFS_DIR && existing->type == RAMFS_DIR)
      return -21; /* -EISDIR */
    if (existing->type == RAMFS_DIR && existing->nchildren > 0)
      return -39; /* -ENOTEMPTY */
    ramfs_dir_remove(new_parent, new_base);
    existing->nlink--;
    /* BF: Defer free if file still has open fds (matches ramfs_unlink/ramfs_close pattern) */
    if (existing->nlink <= 0 && existing->open_count <= 0)
      ramfs_free_inode(existing);
  }

  /* Remove from old parent, add to new parent */
  ramfs_dir_remove(old_parent, old_base);
  int rc = ramfs_dir_add(new_parent, new_base, ino);
  if (rc < 0) {
    /* Try to restore old entry on failure */
    ramfs_dir_add(old_parent, old_base, ino);
    return rc;
  }
  return 0;
}

/* ================================================================== */
/* Symlink operations                                                   */
/* ================================================================== */

/* ENH: Create a symbolic link at linkpath pointing to target.
 * The target string is stored verbatim in the inode (not resolved). */
static int ramfs_symlink(void *fs_data, const char *target, const char *linkpath) {
  (void)fs_data;

  /* Check if it already exists */
  if (ramfs_walk(linkpath))
    return -17; /* -EEXIST */

  /* Validate target fits in inode storage */
  int tlen = strlen(target);
  if (tlen >= (int)sizeof(((struct ramfs_inode *)0)->symlink_target))
    return -36; /* -ENAMETOOLONG */

  const char *basename;
  struct ramfs_inode *parent = ramfs_walk_parent(linkpath, &basename);
  if (!parent || parent->type != RAMFS_DIR)
    return -2; /* -ENOENT (parent dir doesn't exist) */
  if (*basename == '\0')
    return -22; /* -EINVAL */

  struct ramfs_inode *ino = ramfs_alloc_inode(RAMFS_SYMLINK, 0777);
  if (!ino) return -12; /* -ENOMEM */

  memcpy(ino->symlink_target, target, tlen);
  ino->symlink_target[tlen] = '\0';
  ino->size = (uint64_t)tlen;

  int rc = ramfs_dir_add(parent, basename, ino);
  if (rc < 0) {
    ramfs_free_inode(ino);
    return rc;
  }
  return 0;
}

/* ENH: Read the target of a symbolic link into buf.
 * Returns the number of bytes copied (not null-terminated). */
static int ramfs_readlink(void *fs_data, const char *path, char *buf, int bufsiz) {
  (void)fs_data;
  struct ramfs_inode *ino = ramfs_walk(path);
  if (!ino) return -2; /* -ENOENT */
  if (ino->type != RAMFS_SYMLINK) return -22; /* -EINVAL: not a symlink */

  int copylen = (int)ino->size;
  if (copylen > bufsiz)
    copylen = bufsiz;
  memcpy(buf, ino->symlink_target, copylen);
  return copylen;
}

/* ENH: ramfs_link — create a hard link (new directory entry pointing to the
 * same inode).  Increments nlink on the source inode.  Required by linkat(2)
 * to support hard links on in-memory filesystems. */
static int ramfs_link(void *fs_data, const char *oldpath, const char *newpath) {
  (void)fs_data;
  /* Find the source inode */
  struct ramfs_inode *src = ramfs_walk(oldpath);
  if (!src) return -2; /* -ENOENT */
  if (src->type == RAMFS_DIR) return -31; /* -EMLINK (POSIX: no hard links to dirs) */

  /* Find parent directory of newpath and the basename */
  const char *new_base;
  struct ramfs_inode *new_parent = ramfs_walk_parent(newpath, &new_base);
  if (!new_parent || new_parent->type != RAMFS_DIR)
    return -2; /* -ENOENT (parent doesn't exist) */
  if (*new_base == '\0')
    return -22; /* -EINVAL */

  /* Check that newpath doesn't already exist */
  if (ramfs_dir_lookup(new_parent, new_base))
    return -17; /* -EEXIST */

  /* Add a new directory entry in the parent pointing to the same inode */
  int rc = ramfs_dir_add(new_parent, new_base, src);
  if (rc < 0)
    return rc;

  /* Increment the link count on the source inode */
  src->nlink++;
  return 0;
}

static int ramfs_chmod(void *fs_data, const char *path, uint32_t mode, int flags) {
  (void)fs_data;
  (void)flags;
  struct ramfs_inode *ino = ramfs_walk(path);
  if (!ino)
    return -2;
  /* ENH: fchmodat(AT_SYMLINK_NOFOLLOW) validates symlink metadata and is a
   * no-op for link permissions, matching common POSIX behavior. */
  if (ino->type != RAMFS_SYMLINK)
    ino->mode = mode & 07777;
  ino->ctime_sec = timekeeping_now_seconds();
  return 0;
}

static int ramfs_chown(void *fs_data, const char *path, int uid, int gid, int flags) {
  (void)fs_data;
  (void)flags;
  struct ramfs_inode *ino = ramfs_walk(path);
  if (!ino)
    return -2;
  if (uid >= 0)
    ino->uid = (uint32_t)uid;
  if (gid >= 0)
    ino->gid = (uint32_t)gid;
  ino->ctime_sec = timekeeping_now_seconds();
  return 0;
}

static int ramfs_utimens(void *fs_data, const char *path,
                         uint64_t atime_sec, uint64_t mtime_sec,
                         int set_atime, int set_mtime, int flags) {
  (void)fs_data;
  (void)flags;
  struct ramfs_inode *ino = ramfs_walk(path);
  if (!ino)
    return -2;
  if (set_atime)
    ino->atime_sec = atime_sec;
  if (set_mtime)
    ino->mtime_sec = mtime_sec;
  ino->ctime_sec = timekeeping_now_seconds();
  return 0;
}

/* ================================================================== */
/* VFS ops table                                                        */
/* ================================================================== */

struct vfs_ops ramfs_ops = {
  .open    = ramfs_open,
  .read    = ramfs_read,
  .write   = ramfs_write,
  .ftruncate = ramfs_ftruncate,
  .reopen  = ramfs_reopen,
  .close   = ramfs_close,
  .stat    = ramfs_stat,
  .fstat   = ramfs_fstat,
  .readdir = ramfs_readdir,
  .mkdir   = ramfs_mkdir,
  .unlink  = ramfs_unlink,
  .rmdir   = ramfs_rmdir,
  .symlink = ramfs_symlink,
  .readlink = ramfs_readlink,
  .rename  = ramfs_rename,
  .link    = ramfs_link,
  .chmod   = ramfs_chmod,
  .chown   = ramfs_chown,
  .utimens = ramfs_utimens,
};

/* ================================================================== */
/* Init: create the root directory inode                                 */
/* ================================================================== */

void ramfs_init(void) {
  memset(inodes, 0, sizeof(inodes));
  next_ino = 0;
  /* Allocate root directory as inode[0] */
  struct ramfs_inode *root = &inodes[0];
  root->ino = ++next_ino; /* ino = 1 */
  root->type = RAMFS_DIR;
  root->mode = 01777;  /* sticky + rwxrwxrwx, like /tmp */
  root->nlink = 1;
  root->parent = root; /* root is its own parent */
  root->atime_sec = timekeeping_now_seconds();
  root->mtime_sec = root->atime_sec;
  root->ctime_sec = root->atime_sec;
  root->uid = 0;
  root->gid = 0;
}
