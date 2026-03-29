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

/* ================================================================== */
/* Inode / directory-entry structures                                   */
/* ================================================================== */

#define RAMFS_MAX_INODES   256
#define RAMFS_MAX_CHILDREN  64
#define RAMFS_NAME_MAX      60
#define RAMFS_MAX_BLOCKS    64   /* max 4KB blocks per file = 256KB */

#define RAMFS_INO_ROOT       1

enum ramfs_type { RAMFS_FILE, RAMFS_DIR };

struct ramfs_inode {
  uint64_t         ino;
  enum ramfs_type  type;
  uint32_t         mode;       /* permission bits (no S_IF prefix stored here) */
  int              nlink;
  uint64_t         size;       /* bytes for files, 0 for dirs */
  /* File data blocks (each is a kalloc page = 4096 bytes) */
  char            *blocks[RAMFS_MAX_BLOCKS];
  int              nblocks;
  /* Directory children */
  struct ramfs_dirent {
    char              name[RAMFS_NAME_MAX];
    struct ramfs_inode *inode;
  } children[RAMFS_MAX_CHILDREN];
  int              nchildren;
  struct ramfs_inode *parent;   /* ".." link */
};

static struct ramfs_inode inodes[RAMFS_MAX_INODES];
static uint64_t next_ino = RAMFS_INO_ROOT;

/* ================================================================== */
/* Helpers                                                              */
/* ================================================================== */

static struct ramfs_inode *ramfs_alloc_inode(enum ramfs_type type, uint32_t mode) {
  for (int i = 0; i < RAMFS_MAX_INODES; i++) {
    if (inodes[i].ino == 0) {
      memset(&inodes[i], 0, sizeof(struct ramfs_inode));
      inodes[i].ino = ++next_ino;
      inodes[i].type = type;
      inodes[i].mode = mode;
      inodes[i].nlink = 1;
      return &inodes[i];
    }
  }
  return NULL;
}

static void ramfs_free_inode(struct ramfs_inode *ino) {
  if (!ino) return;
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
    if (strcmp(dir->children[i].name, name) == 0)
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
  struct ramfs_dirent *de = &dir->children[dir->nchildren++];
  memcpy(de->name, name, nlen);
  de->name[nlen] = '\0';
  de->inode = child;
  child->parent = dir;
  return 0;
}

/* Remove a child entry from a directory */
static int ramfs_dir_remove(struct ramfs_inode *dir, const char *name) {
  if (!dir || dir->type != RAMFS_DIR) return -1;
  for (int i = 0; i < dir->nchildren; i++) {
    if (strcmp(dir->children[i].name, name) == 0) {
      /* Swap-remove */
      dir->nchildren--;
      if (i < dir->nchildren)
        dir->children[i] = dir->children[dir->nchildren];
      memset(&dir->children[dir->nchildren], 0, sizeof(struct ramfs_dirent));
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
#define RAMFS_O_TRUNC   0x200
#define RAMFS_O_APPEND  0x400

static int ramfs_open(void *fs_data, const char *path, int flags, void **private) {
  (void)fs_data;
  struct ramfs_inode *ino = ramfs_walk(path);

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

  return (int64_t)total;
}

static int ramfs_close(void *fs_data, void *private) {
  (void)fs_data;
  if (private)
    kmfree(private, sizeof(struct ramfs_private));
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
  else
    mode = (uint32_t)(0x8000 | ino->mode); /* S_IFREG */
  uint32_t nlink = (uint32_t)ino->nlink;
  int64_t size = (int64_t)ino->size;
  int32_t blksz = 4096;
  int64_t blks = ((int64_t)ino->size + 511) / 512;

  memcpy(st + 0, &dev, 8);       /* st_dev */
  memcpy(st + 8, &ino_val, 8);   /* st_ino */
  memcpy(st + 16, &mode, 4);     /* st_mode */
  memcpy(st + 20, &nlink, 4);    /* st_nlink */
  memcpy(st + 48, &size, 8);     /* st_size */
  memcpy(st + 56, &blksz, 4);    /* st_blksize */
  memcpy(st + 64, &blks, 8);     /* st_blocks */
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
    uint8_t dt = (child->type == RAMFS_DIR) ? 4 : 8; /* DT_DIR : DT_REG */
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
  if (ino->type == RAMFS_DIR) return -21; /* -EISDIR */

  ramfs_dir_remove(parent, basename);
  ino->nlink--;
  if (ino->nlink <= 0)
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
  if (ino->nlink <= 0)
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
    if (existing->type == RAMFS_DIR && existing->nchildren > 0)
      return -39; /* -ENOTEMPTY */
    ramfs_dir_remove(new_parent, new_base);
    existing->nlink--;
    if (existing->nlink <= 0)
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
/* VFS ops table                                                        */
/* ================================================================== */

struct vfs_ops ramfs_ops = {
  .open    = ramfs_open,
  .read    = ramfs_read,
  .write   = ramfs_write,
  .close   = ramfs_close,
  .stat    = ramfs_stat,
  .fstat   = ramfs_fstat,
  .readdir = ramfs_readdir,
  .mkdir   = ramfs_mkdir,
  .unlink  = ramfs_unlink,
  .rmdir   = ramfs_rmdir,
  .symlink = NULL,
  .readlink = NULL,
  .rename  = ramfs_rename,
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
}
