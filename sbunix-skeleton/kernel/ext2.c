#include <ext2.h>
#include <vfs.h>
#include <virtio_blk.h>
#include <kalloc.h>
#include <kmalloc.h>
#include <string.h>
#include <printk.h>

/* ================================================================== */
/* Globals                                                             */
/* ================================================================== */
static struct ext2_superblock sb;
static struct bcache_entry bcache[BCACHE_SIZE];
static struct ext2_inode_mem icache[ICACHE_SIZE];
static int ext2_mounted = 0;

/* Clock hand for bcache eviction */
static int bcache_hand = 0;

/* ================================================================== */
/* Block I/O (low-level)                                               */
/* ================================================================== */

static int ext2_read_block(uint32_t blockno, void *buf) {
  return virtio_blk_rw((uint64_t)blockno * EXT2_SECTORS_PER_BLOCK, buf, 0);
}

static int ext2_write_block(uint32_t blockno, const void *buf) {
  return virtio_blk_rw((uint64_t)blockno * EXT2_SECTORS_PER_BLOCK,
                        (void *)buf, 1);
}

/* ================================================================== */
/* Block Cache                                                         */
/* ================================================================== */

static struct bcache_entry *bcache_get(uint32_t blockno) {
  /* Check if already cached */
  for (int i = 0; i < BCACHE_SIZE; i++) {
    if (bcache[i].valid && bcache[i].blockno == blockno) {
      bcache[i].refcount++;
      return &bcache[i];
    }
  }

  /* Find a slot to evict using clock algorithm */
  for (int tries = 0; tries < BCACHE_SIZE * 2; tries++) {
    struct bcache_entry *e = &bcache[bcache_hand];
    bcache_hand = (bcache_hand + 1) % BCACHE_SIZE;

    if (e->refcount > 0)
      continue;

    /* Prefer clean entries */
    if (e->valid && e->dirty) {
      /* Write back dirty entry */
      ext2_write_block(e->blockno, e->data);
      e->dirty = 0;
    }

    /* Evict this slot */
    e->blockno = blockno;
    e->valid = 0;
    e->dirty = 0;
    e->refcount = 1;

    /* Read from disk */
    if (ext2_read_block(blockno, e->data) < 0) {
      e->refcount = 0;
      return NULL;
    }
    e->valid = 1;
    return e;
  }

  printk("ext2: bcache full, no evictable entry\n");
  return NULL;
}

static void bcache_release(struct bcache_entry *e) {
  if (e && e->refcount > 0)
    e->refcount--;
}

static void bcache_mark_dirty(struct bcache_entry *e) {
  if (e)
    e->dirty = 1;
}

static void bcache_sync(void) {
  for (int i = 0; i < BCACHE_SIZE; i++) {
    if (bcache[i].valid && bcache[i].dirty) {
      ext2_write_block(bcache[i].blockno, bcache[i].data);
      bcache[i].dirty = 0;
    }
  }
}

/* ================================================================== */
/* Superblock helpers                                                  */
/* ================================================================== */

static int sb_write(void) {
  struct bcache_entry *e = bcache_get(0);
  if (!e) return -1;
  memcpy(e->data, &sb, 52); /* only copy the on-disk portion */
  memset(e->data + 52, 0, EXT2_BLOCK_SIZE - 52);
  bcache_mark_dirty(e);
  bcache_release(e);
  sb.dirty = 0;
  return 0;
}

static void sb_flush_if_dirty(void) {
  if (sb.dirty)
    sb_write();
}

/* ================================================================== */
/* Block Bitmap                                                        */
/* ================================================================== */

static int ext2_alloc_block(void) {
  /* Scan bitmap for a free bit starting from data_start */
  for (uint32_t bm_block = 0; bm_block < sb.bitmap_blocks; bm_block++) {
    struct bcache_entry *e = bcache_get(sb.bitmap_start + bm_block);
    if (!e) return -1;

    uint32_t base = bm_block * (EXT2_BLOCK_SIZE * 8);
    uint32_t start_bit = 0;
    if (base < sb.data_start)
      start_bit = sb.data_start - base;

    for (uint32_t byte = start_bit / 8; byte < EXT2_BLOCK_SIZE; byte++) {
      if (e->data[byte] == 0xff)
        continue;
      for (int bit = (byte == start_bit / 8 ? start_bit % 8 : 0);
           bit < 8; bit++) {
        if (!(e->data[byte] & (1 << bit))) {
          uint32_t blockno = base + byte * 8 + bit;
          if (blockno >= sb.total_blocks) {
            bcache_release(e);
            return -1;
          }
          e->data[byte] |= (1 << bit);
          bcache_mark_dirty(e);
          bcache_release(e);
          sb.free_blocks--;
          sb.dirty = 1;
          sb_flush_if_dirty();
          return (int)blockno;
        }
      }
    }
    bcache_release(e);
  }
  return -1; /* disk full */
}

static void ext2_free_block(uint32_t blockno) {
  uint32_t bm_block = blockno / (EXT2_BLOCK_SIZE * 8);
  uint32_t bit_idx  = blockno % (EXT2_BLOCK_SIZE * 8);
  uint32_t byte_idx = bit_idx / 8;
  uint32_t bit_off  = bit_idx % 8;

  struct bcache_entry *e = bcache_get(sb.bitmap_start + bm_block);
  if (!e) return;
  e->data[byte_idx] &= ~(1 << bit_off);
  bcache_mark_dirty(e);
  bcache_release(e);
  sb.free_blocks++;
  sb.dirty = 1;
}

/* ================================================================== */
/* Inode Table I/O                                                     */
/* ================================================================== */

static int ext2_read_dinode(uint32_t ino, struct ext2_dinode *di) {
  uint32_t block = sb.itable_start + (ino * EXT2_INODE_SIZE) / EXT2_BLOCK_SIZE;
  uint32_t off   = (ino * EXT2_INODE_SIZE) % EXT2_BLOCK_SIZE;

  struct bcache_entry *e = bcache_get(block);
  if (!e) return -1;
  memcpy(di, e->data + off, sizeof(struct ext2_dinode));
  bcache_release(e);
  return 0;
}

static int ext2_write_dinode(uint32_t ino, struct ext2_dinode *di) {
  uint32_t block = sb.itable_start + (ino * EXT2_INODE_SIZE) / EXT2_BLOCK_SIZE;
  uint32_t off   = (ino * EXT2_INODE_SIZE) % EXT2_BLOCK_SIZE;

  struct bcache_entry *e = bcache_get(block);
  if (!e) return -1;
  memcpy(e->data + off, di, sizeof(struct ext2_dinode));
  bcache_mark_dirty(e);
  bcache_release(e);
  return 0;
}

/* ================================================================== */
/* Inode Cache                                                         */
/* ================================================================== */

static struct ext2_inode_mem *icache_get(uint32_t ino) {
  /* Check if already cached */
  for (int i = 0; i < ICACHE_SIZE; i++) {
    if (icache[i].valid && icache[i].ino == ino) {
      icache[i].refcount++;
      return &icache[i];
    }
  }

  /* Find a free or evictable slot */
  int slot = -1;
  for (int i = 0; i < ICACHE_SIZE; i++) {
    if (!icache[i].valid) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    for (int i = 0; i < ICACHE_SIZE; i++) {
      if (icache[i].refcount == 0) {
        /* Write back if dirty before evicting */
        if (icache[i].dirty) {
          struct ext2_dinode di;
          memset(&di, 0, sizeof(di));
          di.type = icache[i].type;
          di.link_count = icache[i].link_count;
          di.size = icache[i].size;
          memcpy(di.direct, icache[i].direct, sizeof(di.direct));
          di.indirect = icache[i].indirect;
          di.mode = icache[i].mode;
          di.rdev = icache[i].rdev;
          di.ctime = icache[i].ctime;
          di.mtime = icache[i].mtime;
          ext2_write_dinode(icache[i].ino, &di);
        }
        slot = i;
        break;
      }
    }
  }
  if (slot < 0) {
    printk("ext2: icache full\n");
    return NULL;
  }

  /* Read from disk */
  struct ext2_dinode di;
  if (ext2_read_dinode(ino, &di) < 0)
    return NULL;

  struct ext2_inode_mem *ip = &icache[slot];
  ip->ino = ino;
  ip->valid = 1;
  ip->dirty = 0;
  ip->refcount = 1;
  ip->type = di.type;
  ip->link_count = di.link_count;
  ip->size = di.size;
  memcpy(ip->direct, di.direct, sizeof(ip->direct));
  ip->indirect = di.indirect;
  ip->mode = di.mode;
  ip->rdev = di.rdev;
  ip->ctime = di.ctime;
  ip->mtime = di.mtime;
  return ip;
}

static void icache_release(struct ext2_inode_mem *ip) {
  if (ip && ip->refcount > 0)
    ip->refcount--;
}

static void icache_writeback(struct ext2_inode_mem *ip) {
  if (!ip || !ip->dirty)
    return;
  struct ext2_dinode di;
  memset(&di, 0, sizeof(di));
  di.type = ip->type;
  di.link_count = ip->link_count;
  di.size = ip->size;
  memcpy(di.direct, ip->direct, sizeof(di.direct));
  di.indirect = ip->indirect;
  di.mode = ip->mode;
  di.rdev = ip->rdev;
  ext2_write_dinode(ip->ino, &di);
  ip->dirty = 0;
}

static void icache_evict(uint32_t ino) {
  for (int i = 0; i < ICACHE_SIZE; i++) {
    if (icache[i].valid && icache[i].ino == ino) {
      icache[i].valid = 0;
      icache[i].refcount = 0;
      icache[i].dirty = 0;
      return;
    }
  }
}

/* ================================================================== */
/* Inode Allocation                                                    */
/* ================================================================== */

static int ext2_alloc_inode(uint16_t type) {
  for (uint32_t ino = 0; ino < sb.total_inodes; ino++) {
    struct ext2_dinode di;
    if (ext2_read_dinode(ino, &di) < 0)
      continue;
    if (di.type == EXT2_TYPE_FREE) {
      memset(&di, 0, sizeof(di));
      di.type = type;
      di.link_count = 1;
      if (type == EXT2_TYPE_DIR)
        di.mode = 0755;
      else if (type == EXT2_TYPE_CHRDEV || type == EXT2_TYPE_BLKDEV)
        di.mode = 0666;
      else if (type == EXT2_TYPE_SYMLINK)
        di.mode = 0777;
      else
        di.mode = 0644;
      ext2_write_dinode(ino, &di);
      sb.free_inodes--;
      sb.dirty = 1;
      sb_flush_if_dirty();
      return (int)ino;
    }
  }
  return -1;
}

static void ext2_free_inode(uint32_t ino) {
  struct ext2_inode_mem *ip = icache_get(ino);
  if (!ip) {
    /* Read directly */
    struct ext2_dinode di;
    if (ext2_read_dinode(ino, &di) < 0)
      return;
    /* Free direct blocks */
    for (int i = 0; i < EXT2_NDIRECT; i++) {
      if (di.direct[i])
        ext2_free_block(di.direct[i]);
    }
    /* Free indirect blocks */
    if (di.indirect) {
      struct bcache_entry *e = bcache_get(di.indirect);
      if (e) {
        uint32_t *ptrs = (uint32_t *)e->data;
        for (int i = 0; i < EXT2_NINDIRECT; i++) {
          if (ptrs[i])
            ext2_free_block(ptrs[i]);
        }
        bcache_release(e);
      }
      ext2_free_block(di.indirect);
    }
    /* Zero the inode */
    memset(&di, 0, sizeof(di));
    ext2_write_dinode(ino, &di);
  } else {
    /* Free direct blocks */
    for (int i = 0; i < EXT2_NDIRECT; i++) {
      if (ip->direct[i])
        ext2_free_block(ip->direct[i]);
    }
    /* Free indirect blocks */
    if (ip->indirect) {
      struct bcache_entry *e = bcache_get(ip->indirect);
      if (e) {
        uint32_t *ptrs = (uint32_t *)e->data;
        for (int i = 0; i < EXT2_NINDIRECT; i++) {
          if (ptrs[i])
            ext2_free_block(ptrs[i]);
        }
        bcache_release(e);
      }
      ext2_free_block(ip->indirect);
    }
    icache_release(ip);
    icache_evict(ino);
    /* Zero the inode on disk */
    struct ext2_dinode di;
    memset(&di, 0, sizeof(di));
    ext2_write_dinode(ino, &di);
  }
  sb.free_inodes++;
  sb.dirty = 1;
}

/* ================================================================== */
/* Block Mapping (bmap)                                                */
/* ================================================================== */

static uint32_t ext2_bmap(struct ext2_inode_mem *ip, uint32_t file_block) {
  if (file_block < EXT2_NDIRECT) {
    return ip->direct[file_block];
  }
  uint32_t idx = file_block - EXT2_NDIRECT;
  if (idx >= EXT2_NINDIRECT)
    return 0;
  if (ip->indirect == 0)
    return 0;
  struct bcache_entry *e = bcache_get(ip->indirect);
  if (!e) return 0;
  uint32_t *ptrs = (uint32_t *)e->data;
  uint32_t blk = ptrs[idx];
  bcache_release(e);
  return blk;
}

/* bmap_alloc: like bmap but allocates blocks on demand.
 * Returns disk block number, or 0 on failure. */
static uint32_t ext2_bmap_alloc(struct ext2_inode_mem *ip, uint32_t file_block) {
  if (file_block < EXT2_NDIRECT) {
    if (ip->direct[file_block] == 0) {
      int blk = ext2_alloc_block();
      if (blk < 0) return 0;
      ip->direct[file_block] = (uint32_t)blk;
      ip->dirty = 1;
      /* Zero the new block */
      struct bcache_entry *e = bcache_get((uint32_t)blk);
      if (e) {
        memset(e->data, 0, EXT2_BLOCK_SIZE);
        bcache_mark_dirty(e);
        bcache_release(e);
      }
    }
    return ip->direct[file_block];
  }

  uint32_t idx = file_block - EXT2_NDIRECT;
  if (idx >= EXT2_NINDIRECT)
    return 0;

  /* Allocate indirect block if needed */
  if (ip->indirect == 0) {
    int blk = ext2_alloc_block();
    if (blk < 0) return 0;
    ip->indirect = (uint32_t)blk;
    ip->dirty = 1;
    /* Zero the indirect block */
    struct bcache_entry *e = bcache_get((uint32_t)blk);
    if (e) {
      memset(e->data, 0, EXT2_BLOCK_SIZE);
      bcache_mark_dirty(e);
      bcache_release(e);
    }
  }

  struct bcache_entry *e = bcache_get(ip->indirect);
  if (!e) return 0;
  uint32_t *ptrs = (uint32_t *)e->data;
  if (ptrs[idx] == 0) {
    int blk = ext2_alloc_block();
    if (blk < 0) {
      bcache_release(e);
      return 0;
    }
    ptrs[idx] = (uint32_t)blk;
    bcache_mark_dirty(e);
    /* Zero the new data block */
    struct bcache_entry *e2 = bcache_get((uint32_t)blk);
    if (e2) {
      memset(e2->data, 0, EXT2_BLOCK_SIZE);
      bcache_mark_dirty(e2);
      bcache_release(e2);
    }
  }
  uint32_t result = ptrs[idx];
  bcache_release(e);
  return result;
}

/* ================================================================== */
/* Directory Operations                                                */
/* ================================================================== */

/* Minimum dirent size: 4 (ino) + 2 (rec_len) + 1 (name_len) + 1 (type) = 8 */
#define EXT2_DIRENT_MIN 8

/* Compute actual size needed for a directory entry with given name_len */
static uint32_t ext2_dirent_size(uint32_t name_len) {
  /* 8 bytes header + name_len, rounded up to 4 */
  return (EXT2_DIRENT_MIN + name_len + 3) & ~3u;
}

static int ext2_dir_lookup(struct ext2_inode_mem *dir, const char *name,
                           uint32_t *out_ino) {
  if (dir->type != EXT2_TYPE_DIR)
    return -1;

  uint32_t name_len = strlen(name);
  uint32_t nblocks = (dir->size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t disk_blk = ext2_bmap(dir, b);
    if (disk_blk == 0) continue;

    struct bcache_entry *e = bcache_get(disk_blk);
    if (!e) continue;

    uint32_t off = 0;
    while (off < EXT2_BLOCK_SIZE) {
      struct ext2_dirent *de = (struct ext2_dirent *)(e->data + off);
      if (de->rec_len == 0) break;

      if (de->inode_number != 0 &&
          de->name_len == name_len &&
          memcmp(de->name, name, name_len) == 0) {
        *out_ino = de->inode_number;
        bcache_release(e);
        return 0;
      }
      off += de->rec_len;
    }
    bcache_release(e);
  }
  return -2; /* -ENOENT */
}

static int ext2_dir_add(struct ext2_inode_mem *dir, const char *name,
                        uint32_t ino, uint8_t file_type) {
  uint32_t name_len = strlen(name);
  uint32_t needed = ext2_dirent_size(name_len);
  uint32_t nblocks = (dir->size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

  /* Try to find space in existing blocks */
  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t disk_blk = ext2_bmap(dir, b);
    if (disk_blk == 0) continue;

    struct bcache_entry *e = bcache_get(disk_blk);
    if (!e) continue;

    uint32_t off = 0;
    while (off < EXT2_BLOCK_SIZE) {
      struct ext2_dirent *de = (struct ext2_dirent *)(e->data + off);
      if (de->rec_len == 0) break;

      if (de->inode_number == 0 && de->rec_len >= needed) {
        /* Reuse deleted entry */
        de->inode_number = ino;
        de->name_len = (uint8_t)name_len;
        de->file_type = file_type;
        memcpy(de->name, name, name_len);
        bcache_mark_dirty(e);
        bcache_release(e);
        return 0;
      }

      /* Check if there's slack space at the end of this entry */
      uint32_t actual_size = ext2_dirent_size(de->name_len);
      if (de->inode_number != 0 && de->rec_len >= actual_size + needed) {
        /* Split: shrink current entry, add new one in slack */
        uint32_t new_rec_len = de->rec_len - actual_size;
        de->rec_len = (uint16_t)actual_size;

        struct ext2_dirent *new_de =
          (struct ext2_dirent *)(e->data + off + actual_size);
        new_de->inode_number = ino;
        new_de->rec_len = (uint16_t)new_rec_len;
        new_de->name_len = (uint8_t)name_len;
        new_de->file_type = file_type;
        memcpy(new_de->name, name, name_len);
        bcache_mark_dirty(e);
        bcache_release(e);
        return 0;
      }

      off += de->rec_len;
    }
    bcache_release(e);
  }

  /* No space found in existing blocks -- allocate a new one */
  uint32_t new_blk = ext2_bmap_alloc(dir, nblocks);
  if (new_blk == 0)
    return -28; /* -ENOSPC */

  struct bcache_entry *e = bcache_get(new_blk);
  if (!e) return -1;

  memset(e->data, 0, EXT2_BLOCK_SIZE);
  struct ext2_dirent *de = (struct ext2_dirent *)e->data;
  de->inode_number = ino;
  de->rec_len = EXT2_BLOCK_SIZE; /* last entry spans to end of block */
  de->name_len = (uint8_t)name_len;
  de->file_type = file_type;
  memcpy(de->name, name, name_len);
  bcache_mark_dirty(e);
  bcache_release(e);

  dir->size += EXT2_BLOCK_SIZE;
  dir->dirty = 1;
  return 0;
}

static int ext2_dir_remove(struct ext2_inode_mem *dir, const char *name) {
  uint32_t name_len = strlen(name);
  uint32_t nblocks = (dir->size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t disk_blk = ext2_bmap(dir, b);
    if (disk_blk == 0) continue;

    struct bcache_entry *e = bcache_get(disk_blk);
    if (!e) continue;

    uint32_t off = 0;
    struct ext2_dirent *prev = NULL;
    while (off < EXT2_BLOCK_SIZE) {
      struct ext2_dirent *de = (struct ext2_dirent *)(e->data + off);
      if (de->rec_len == 0) break;

      if (de->inode_number != 0 &&
          de->name_len == name_len &&
          memcmp(de->name, name, name_len) == 0) {
        /* Found it */
        if (prev) {
          /* Merge rec_len into previous entry */
          prev->rec_len += de->rec_len;
        }
        de->inode_number = 0;
        bcache_mark_dirty(e);
        bcache_release(e);
        return 0;
      }
      prev = de;
      off += de->rec_len;
    }
    bcache_release(e);
  }
  return -2; /* -ENOENT */
}

/* ================================================================== */
/* Path Resolution                                                     */
/* ================================================================== */

/* Split path into parent dir path and base name.
 * Returns pointer to basename within path.
 * parent_out receives the parent directory path. */
static const char *path_parent(const char *path, char *parent_out,
                               int parent_size) {
  int len = strlen(path);
  /* Remove trailing slashes */
  while (len > 0 && path[len - 1] == '/')
    len--;

  /* Find last slash */
  int last_slash = -1;
  for (int i = len - 1; i >= 0; i--) {
    if (path[i] == '/') {
      last_slash = i;
      break;
    }
  }

  const char *basename;
  if (last_slash < 0) {
    /* No slash, parent is root */
    parent_out[0] = '\0';
    basename = path;
  } else if (last_slash == 0) {
    /* Root is the parent */
    parent_out[0] = '/';
    parent_out[1] = '\0';
    basename = path + 1;
  } else {
    int plen = last_slash;
    if (plen >= parent_size)
      plen = parent_size - 1;
    memcpy(parent_out, path, plen);
    parent_out[plen] = '\0';
    basename = path + last_slash + 1;
  }

  return basename;
}

/* Resolve path relative to ext2 root.
 * Returns inode with refcount++ on success.
 * The path is relative to the /mnt mount point (VFS strips prefix). */
static struct ext2_inode_mem *ext2_namei(const char *path) {
  /* Start from root inode */
  struct ext2_inode_mem *ip = icache_get(EXT2_ROOT_INO);
  if (!ip) return NULL;

  /* Handle root path */
  if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
    return ip;

  const char *p = path;
  while (*p == '/') p++;

  while (*p != '\0') {
    /* Collect next path component */
    const char *start = p;
    while (*p != '\0' && *p != '/')
      p++;
    int clen = (int)(p - start);
    if (clen == 0) {
      /* Skip consecutive slashes */
      while (*p == '/') p++;
      continue;
    }

    if (ip->type != EXT2_TYPE_DIR) {
      icache_release(ip);
      return NULL;
    }

    /* Copy component to a null-terminated buffer */
    char comp[256];
    if (clen > 255) clen = 255;
    memcpy(comp, start, clen);
    comp[clen] = '\0';

    uint32_t child_ino;
    if (ext2_dir_lookup(ip, comp, &child_ino) < 0) {
      icache_release(ip);
      return NULL;
    }

    icache_release(ip);
    ip = icache_get(child_ino);
    if (!ip) return NULL;

    /* Skip trailing slashes */
    while (*p == '/') p++;
  }

  return ip;
}

/* ================================================================== */
/* Truncate -- free all data blocks of an inode                        */
/* ================================================================== */

static void ext2_truncate(struct ext2_inode_mem *ip) {
  /* Free direct blocks */
  for (int i = 0; i < EXT2_NDIRECT; i++) {
    if (ip->direct[i]) {
      ext2_free_block(ip->direct[i]);
      ip->direct[i] = 0;
    }
  }
  /* Free indirect blocks */
  if (ip->indirect) {
    struct bcache_entry *e = bcache_get(ip->indirect);
    if (e) {
      uint32_t *ptrs = (uint32_t *)e->data;
      for (int i = 0; i < EXT2_NINDIRECT; i++) {
        if (ptrs[i])
          ext2_free_block(ptrs[i]);
      }
      bcache_release(e);
    }
    ext2_free_block(ip->indirect);
    ip->indirect = 0;
  }
  ip->size = 0;
  ip->dirty = 1;
}

/* ================================================================== */
/* VFS Ops Implementation                                              */
/* ================================================================== */

static int ext2_open(const char *path, int flags, void **private_out) {
  if (!ext2_mounted)
    return -1;

  /* Strip leading slashes for namei */
  while (*path == '/') path++;

  struct ext2_inode_mem *ip = ext2_namei(path);

  if (!ip && (flags & EXT2_O_CREAT)) {
    /* Create a new file */
    /* Find parent directory */
    char parent_path[256];
    const char *basename = path_parent(path, parent_path, sizeof(parent_path));
    if (basename[0] == '\0')
      return -1;

    struct ext2_inode_mem *parent = ext2_namei(parent_path);
    if (!parent)
      return -2; /* -ENOENT */
    if (parent->type != EXT2_TYPE_DIR) {
      icache_release(parent);
      return -20; /* -ENOTDIR */
    }

    /* Allocate new inode */
    int new_ino = ext2_alloc_inode(EXT2_TYPE_FILE);
    if (new_ino < 0) {
      icache_release(parent);
      return -28; /* -ENOSPC */
    }

    /* Add directory entry */
    int ret = ext2_dir_add(parent, basename, (uint32_t)new_ino, EXT2_FT_FILE);
    icache_writeback(parent);
    icache_release(parent);
    if (ret < 0) {
      ext2_free_inode((uint32_t)new_ino);
      return ret;
    }

    ip = icache_get((uint32_t)new_ino);
    if (!ip) return -1;
  }

  if (!ip)
    return -2; /* -ENOENT */

  /* Handle O_TRUNC */
  if ((flags & EXT2_O_TRUNC) && ip->type == EXT2_TYPE_FILE) {
    ext2_truncate(ip);
    icache_writeback(ip);
  }

  /* Allocate private data */
  struct ext2_open_private *priv =
    (struct ext2_open_private *)kmalloc(sizeof(struct ext2_open_private));
  if (!priv) {
    icache_release(ip);
    return -12; /* -ENOMEM */
  }
  priv->ino = ip->ino;
  priv->is_dir = (ip->type == EXT2_TYPE_DIR);
  /* Keep refcount held from icache_get */

  *private_out = priv;
  return 0;
}

static int64_t ext2_read(void *private, char *buf, uint64_t count,
                         int64_t *offset) {
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv) return -1;

  struct ext2_inode_mem *ip = icache_get(priv->ino);
  if (!ip) return -1;

  uint64_t off = (uint64_t)*offset;
  if (off >= ip->size) {
    icache_release(ip);
    return 0;
  }

  uint64_t avail = ip->size - off;
  if (count > avail) count = avail;

  uint64_t total = 0;
  while (total < count) {
    uint32_t file_block = (uint32_t)((off + total) / EXT2_BLOCK_SIZE);
    uint32_t block_off  = (uint32_t)((off + total) % EXT2_BLOCK_SIZE);
    uint32_t disk_blk = ext2_bmap(ip, file_block);

    uint64_t chunk = EXT2_BLOCK_SIZE - block_off;
    if (chunk > count - total) chunk = count - total;

    if (disk_blk == 0) {
      /* Hole: zero-fill */
      memset(buf + total, 0, chunk);
    } else {
      struct bcache_entry *e = bcache_get(disk_blk);
      if (!e) break;
      memcpy(buf + total, e->data + block_off, chunk);
      bcache_release(e);
    }
    total += chunk;
  }

  *offset += (int64_t)total;
  icache_release(ip);
  return (int64_t)total;
}

static int64_t ext2_write(void *private, const char *buf, uint64_t count,
                          int64_t *offset) {
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv) return -1;

  struct ext2_inode_mem *ip = icache_get(priv->ino);
  if (!ip) return -1;

  uint64_t off = (uint64_t)*offset;
  uint64_t total = 0;

  while (total < count) {
    uint32_t file_block = (uint32_t)((off + total) / EXT2_BLOCK_SIZE);
    uint32_t block_off  = (uint32_t)((off + total) % EXT2_BLOCK_SIZE);

    /* Check maximum file size */
    if (file_block >= EXT2_NDIRECT + EXT2_NINDIRECT) {
      if (total == 0) {
        icache_release(ip);
        return -27; /* -EFBIG */
      }
      break;
    }

    uint32_t disk_blk = ext2_bmap_alloc(ip, file_block);
    if (disk_blk == 0) {
      if (total == 0) {
        icache_release(ip);
        return -28; /* -ENOSPC */
      }
      break;
    }

    struct bcache_entry *e = bcache_get(disk_blk);
    if (!e) break;

    uint64_t chunk = EXT2_BLOCK_SIZE - block_off;
    if (chunk > count - total) chunk = count - total;

    memcpy(e->data + block_off, buf + total, chunk);
    bcache_mark_dirty(e);
    bcache_release(e);
    total += chunk;
  }

  /* Update size if we extended the file */
  if (off + total > ip->size) {
    ip->size = (uint32_t)(off + total);
    ip->dirty = 1;
  }

  *offset += (int64_t)total;
  icache_writeback(ip);
  icache_release(ip);
  return (int64_t)total;
}

static int ext2_close(void *private) {
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv) return 0;

  struct ext2_inode_mem *ip = icache_get(priv->ino);
  if (ip) {
    /* We got a new ref from icache_get, plus there was one from open.
     * Release both: the open-time ref and this lookup ref. */
    icache_release(ip); /* release our lookup ref */
    icache_release(ip); /* release the open-time ref */

    if (ip->refcount == 0 && ip->link_count == 0 &&
        ip->ino != EXT2_ROOT_INO) {
      ext2_free_inode(ip->ino);
    } else {
      icache_writeback(ip);
    }
  }

  kmfree(priv, sizeof(struct ext2_open_private));
  return 0;
}

static int ext2_stat(const char *path, uint8_t *st) {
  if (!ext2_mounted)
    return -1;

  while (*path == '/') path++;

  struct ext2_inode_mem *ip = ext2_namei(path);
  if (!ip) return -2; /* -ENOENT */

  memset(st, 0, 128);
  uint64_t dev = 2; /* ext2 device */
  uint64_t ino = (uint64_t)ip->ino;
  uint32_t mode;
  if (ip->type == EXT2_TYPE_DIR)
    mode = (uint32_t)(0x4000 | (ip->mode & 0xfff));
  else if (ip->type == EXT2_TYPE_CHRDEV)
    mode = (uint32_t)(0x2000 | (ip->mode & 0xfff));
  else if (ip->type == EXT2_TYPE_BLKDEV)
    mode = (uint32_t)(0x6000 | (ip->mode & 0xfff));
  else if (ip->type == EXT2_TYPE_SYMLINK)
    mode = (uint32_t)(0xA000 | (ip->mode & 0xfff));
  else
    mode = (uint32_t)(0x8000 | (ip->mode & 0xfff));
  uint32_t nlink = ip->link_count;
  int64_t size = (int64_t)ip->size;
  int32_t blksz = 4096;
  int64_t blks = ((int64_t)ip->size + 511) / 512;
  uint64_t rdev = (uint64_t)ip->rdev;

  memcpy(st + 0, &dev, 8);
  memcpy(st + 8, &ino, 8);
  memcpy(st + 16, &mode, 4);
  memcpy(st + 20, &nlink, 4);
  /* uid=0, gid=0 already zeroed */
  memcpy(st + 32, &rdev, 8);  /* st_rdev */
  memcpy(st + 48, &size, 8);
  memcpy(st + 56, &blksz, 4);
  memcpy(st + 64, &blks, 8);
  /* timestamps */
  int64_t mtime_sec = (int64_t)ip->mtime;
  int64_t ctime_sec = (int64_t)ip->ctime;
  memcpy(st + 72, &mtime_sec, 8);   /* st_atime_sec (use mtime as atime) */
  memcpy(st + 88, &mtime_sec, 8);   /* st_mtime_sec */
  memcpy(st + 104, &ctime_sec, 8);  /* st_ctime_sec */

  icache_release(ip);
  return 0;
}

static int ext2_fstat(void *private, uint8_t *st) {
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv) return -1;

  struct ext2_inode_mem *ip = icache_get(priv->ino);
  if (!ip) return -1;

  memset(st, 0, 128);
  uint64_t dev = 2; /* ext2 device */
  uint64_t ino = (uint64_t)ip->ino;
  uint32_t mode;
  if (ip->type == EXT2_TYPE_DIR)
    mode = (uint32_t)(0x4000 | (ip->mode & 0xfff));
  else if (ip->type == EXT2_TYPE_CHRDEV)
    mode = (uint32_t)(0x2000 | (ip->mode & 0xfff));
  else if (ip->type == EXT2_TYPE_BLKDEV)
    mode = (uint32_t)(0x6000 | (ip->mode & 0xfff));
  else if (ip->type == EXT2_TYPE_SYMLINK)
    mode = (uint32_t)(0xA000 | (ip->mode & 0xfff));
  else
    mode = (uint32_t)(0x8000 | (ip->mode & 0xfff));
  uint32_t nlink = ip->link_count;
  int64_t size = (int64_t)ip->size;
  int32_t blksz = 4096;
  int64_t blks = ((int64_t)ip->size + 511) / 512;
  uint64_t rdev = (uint64_t)ip->rdev;

  memcpy(st + 0, &dev, 8);
  memcpy(st + 8, &ino, 8);
  memcpy(st + 16, &mode, 4);
  memcpy(st + 20, &nlink, 4);
  /* uid=0, gid=0 already zeroed */
  memcpy(st + 32, &rdev, 8);  /* st_rdev */
  memcpy(st + 48, &size, 8);
  memcpy(st + 56, &blksz, 4);
  memcpy(st + 64, &blks, 8);
  /* timestamps */
  int64_t mtime_sec = (int64_t)ip->mtime;
  int64_t ctime_sec = (int64_t)ip->ctime;
  memcpy(st + 72, &mtime_sec, 8);   /* st_atime_sec (use mtime as atime) */
  memcpy(st + 88, &mtime_sec, 8);   /* st_mtime_sec */
  memcpy(st + 104, &ctime_sec, 8);  /* st_ctime_sec */

  icache_release(ip);
  return 0;
}

static int ext2_readdir(void *private, char *ubuf, unsigned int count,
                        int64_t *offset) {
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv || !priv->is_dir)
    return -1;

  struct ext2_inode_mem *ip = icache_get(priv->ino);
  if (!ip) return -1;

  uint64_t dir_off = (uint64_t)*offset;
  unsigned int written = 0;
  uint64_t pos = 0;
  uint32_t nblocks = (ip->size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t disk_blk = ext2_bmap(ip, b);
    if (disk_blk == 0) {
      pos += EXT2_BLOCK_SIZE;
      continue;
    }

    struct bcache_entry *e = bcache_get(disk_blk);
    if (!e) {
      pos += EXT2_BLOCK_SIZE;
      continue;
    }

    uint32_t off = 0;
    while (off < EXT2_BLOCK_SIZE) {
      struct ext2_dirent *de = (struct ext2_dirent *)(e->data + off);
      if (de->rec_len == 0) break;

      if (de->inode_number != 0 && pos >= dir_off) {
        /* Emit a linux_dirent64 */
        int nl = de->name_len;
        int rl = (19 + nl + 1 + 7) & ~7;
        if (written + (unsigned int)rl > count) {
          bcache_release(e);
          goto done;
        }

        char *d = ubuf + written;
        memset(d, 0, rl);
        uint64_t d_ino = (uint64_t)de->inode_number;
        int64_t d_off = (int64_t)(pos + de->rec_len);
        uint16_t d_reclen = (uint16_t)rl;
        uint8_t d_type = (de->file_type == EXT2_FT_DIR) ? 4 : 8;

        memcpy(d + 0, &d_ino, 8);
        memcpy(d + 8, &d_off, 8);
        memcpy(d + 16, &d_reclen, 2);
        memcpy(d + 18, &d_type, 1);
        memcpy(d + 19, de->name, nl);
        d[19 + nl] = '\0';

        written += (unsigned int)rl;
        *offset = (int64_t)(pos + de->rec_len);
      }

      pos += de->rec_len;
      off += de->rec_len;
    }
    bcache_release(e);
  }

done:
  icache_release(ip);
  return (int)written;
}

static int ext2_mkdir(const char *path, int mode) {
  if (!ext2_mounted)
    return -1;

  while (*path == '/') path++;

  /* Parse parent and basename */
  char parent_path[256];
  const char *basename = path_parent(path, parent_path, sizeof(parent_path));
  if (basename[0] == '\0')
    return -1;

  struct ext2_inode_mem *parent = ext2_namei(parent_path);
  if (!parent)
    return -2; /* -ENOENT */
  if (parent->type != EXT2_TYPE_DIR) {
    icache_release(parent);
    return -20; /* -ENOTDIR */
  }

  /* Check that name doesn't exist */
  uint32_t existing;
  if (ext2_dir_lookup(parent, basename, &existing) == 0) {
    icache_release(parent);
    return -17; /* -EEXIST */
  }

  /* Allocate new inode for the directory */
  int new_ino = ext2_alloc_inode(EXT2_TYPE_DIR);
  if (new_ino < 0) {
    icache_release(parent);
    return -28; /* -ENOSPC */
  }

  /* Get the new inode into icache */
  struct ext2_inode_mem *ip = icache_get((uint32_t)new_ino);
  if (!ip) {
    ext2_free_inode((uint32_t)new_ino);
    icache_release(parent);
    return -1;
  }

  /* Set mode */
  ip->mode = (uint32_t)(mode & 0xfff);
  if (ip->mode == 0)
    ip->mode = 0755;

  /* Allocate first data block for . and .. entries */
  uint32_t data_blk = ext2_bmap_alloc(ip, 0);
  if (data_blk == 0) {
    icache_release(ip);
    ext2_free_inode((uint32_t)new_ino);
    icache_release(parent);
    return -28; /* -ENOSPC */
  }

  ip->size = EXT2_BLOCK_SIZE;
  ip->dirty = 1;

  /* Write . and .. entries */
  struct bcache_entry *e = bcache_get(data_blk);
  if (!e) {
    icache_release(ip);
    ext2_free_inode((uint32_t)new_ino);
    icache_release(parent);
    return -1;
  }

  memset(e->data, 0, EXT2_BLOCK_SIZE);
  /* "." entry */
  struct ext2_dirent *de = (struct ext2_dirent *)e->data;
  de->inode_number = (uint32_t)new_ino;
  de->rec_len = 12; /* 8 header + 1 name padded to 4 */
  de->name_len = 1;
  de->file_type = EXT2_FT_DIR;
  de->name[0] = '.';

  /* ".." entry -- spans to end of block */
  de = (struct ext2_dirent *)(e->data + 12);
  de->inode_number = parent->ino;
  de->rec_len = (uint16_t)(EXT2_BLOCK_SIZE - 12);
  de->name_len = 2;
  de->file_type = EXT2_FT_DIR;
  de->name[0] = '.';
  de->name[1] = '.';

  bcache_mark_dirty(e);
  bcache_release(e);

  icache_writeback(ip);
  icache_release(ip);

  /* Add entry in parent directory */
  int ret = ext2_dir_add(parent, basename, (uint32_t)new_ino, EXT2_FT_DIR);
  icache_writeback(parent);
  icache_release(parent);

  if (ret < 0) {
    ext2_free_inode((uint32_t)new_ino);
    return ret;
  }

  bcache_sync();
  return 0;
}

static int ext2_unlink(const char *path) {
  if (!ext2_mounted)
    return -1;

  while (*path == '/') path++;

  /* Parse parent and basename */
  char parent_path[256];
  const char *basename = path_parent(path, parent_path, sizeof(parent_path));
  if (basename[0] == '\0')
    return -1;

  struct ext2_inode_mem *parent = ext2_namei(parent_path);
  if (!parent)
    return -2; /* -ENOENT */
  if (parent->type != EXT2_TYPE_DIR) {
    icache_release(parent);
    return -20; /* -ENOTDIR */
  }

  /* Look up the target inode */
  uint32_t target_ino;
  if (ext2_dir_lookup(parent, basename, &target_ino) < 0) {
    icache_release(parent);
    return -2; /* -ENOENT */
  }

  /* Don't allow unlinking root */
  if (target_ino == EXT2_ROOT_INO) {
    icache_release(parent);
    return -1;
  }

  struct ext2_inode_mem *ip = icache_get(target_ino);
  if (!ip) {
    icache_release(parent);
    return -1;
  }

  /* Don't unlink directories (they need rmdir) */
  if (ip->type == EXT2_TYPE_DIR) {
    icache_release(ip);
    icache_release(parent);
    return -21; /* -EISDIR */
  }

  /* Remove directory entry */
  int ret = ext2_dir_remove(parent, basename);
  icache_writeback(parent);
  icache_release(parent);

  if (ret < 0) {
    icache_release(ip);
    return ret;
  }

  /* Decrement link count */
  ip->link_count--;
  ip->dirty = 1;

  if (ip->link_count == 0 && ip->refcount <= 1) {
    /* No more references -- free immediately */
    icache_release(ip);
    ext2_free_inode(target_ino);
  } else {
    icache_writeback(ip);
    icache_release(ip);
  }

  bcache_sync();
  sb_flush_if_dirty();
  return 0;
}

int ext2_mknod(const char *path, uint32_t mode, uint32_t rdev) {
  if (!ext2_mounted)
    return -1;

  while (*path == '/') path++;

  /* Determine inode type from mode */
  uint16_t itype;
  uint8_t ftype;
  if ((mode & 0xF000) == 0x2000) {      /* S_IFCHR */
    itype = EXT2_TYPE_CHRDEV;
    ftype = EXT2_FT_CHRDEV;
  } else if ((mode & 0xF000) == 0x6000) { /* S_IFBLK */
    itype = EXT2_TYPE_BLKDEV;
    ftype = EXT2_FT_BLKDEV;
  } else {
    return -22; /* -EINVAL */
  }

  char parent_path[256];
  const char *basename = path_parent(path, parent_path, sizeof(parent_path));
  if (basename[0] == '\0')
    return -1;

  struct ext2_inode_mem *parent = ext2_namei(parent_path);
  if (!parent)
    return -2; /* -ENOENT */
  if (parent->type != EXT2_TYPE_DIR) {
    icache_release(parent);
    return -20; /* -ENOTDIR */
  }

  /* Check name doesn't exist */
  uint32_t existing;
  if (ext2_dir_lookup(parent, basename, &existing) == 0) {
    icache_release(parent);
    return -17; /* -EEXIST */
  }

  int new_ino = ext2_alloc_inode(itype);
  if (new_ino < 0) {
    icache_release(parent);
    return -28; /* -ENOSPC */
  }

  struct ext2_inode_mem *ip = icache_get((uint32_t)new_ino);
  if (!ip) {
    ext2_free_inode((uint32_t)new_ino);
    icache_release(parent);
    return -1;
  }

  ip->mode = mode & 0xfff;
  ip->rdev = rdev;
  ip->dirty = 1;
  icache_writeback(ip);
  icache_release(ip);

  int ret = ext2_dir_add(parent, basename, (uint32_t)new_ino, ftype);
  icache_writeback(parent);
  icache_release(parent);

  if (ret < 0) {
    ext2_free_inode((uint32_t)new_ino);
    return ret;
  }

  bcache_sync();
  return 0;
}

static int ext2_symlink(const char *target, const char *linkpath) {
    if (!ext2_mounted)
        return -1;

    while (*linkpath == '/') linkpath++;
    if (linkpath[0] == '\0')
        return -1;

    int targetlen = strlen(target);
    if (targetlen == 0 || targetlen >= EXT2_BLOCK_SIZE)
        return -22; /* EINVAL */

    char parent_path[256];
    const char *basename = path_parent(linkpath, parent_path, sizeof(parent_path));
    if (basename[0] == '\0')
        return -1;

    struct ext2_inode_mem *parent = ext2_namei(parent_path);
    if (!parent)
        return -2; /* ENOENT */

    /* Check name doesn't already exist */
    uint32_t existing_ino;
    if (ext2_dir_lookup(parent, basename, &existing_ino) == 0) {
        icache_release(parent);
        return -17; /* EEXIST */
    }

    /* Allocate inode */
    int ino = ext2_alloc_inode(EXT2_TYPE_SYMLINK);
    if (ino < 0) {
        icache_release(parent);
        return -28; /* ENOSPC */
    }

    struct ext2_inode_mem *ip = icache_get((uint32_t)ino);
    if (!ip) {
        icache_release(parent);
        return -1;
    }

    ip->mode = 0777;
    ip->size = (uint32_t)targetlen;

    /* Allocate data block and write target */
    uint32_t blk = ext2_bmap_alloc(ip, 0);
    if (blk == 0) {
        icache_release(ip);
        icache_release(parent);
        return -28; /* ENOSPC */
    }

    struct bcache_entry *be = bcache_get(blk);
    memset(be->data, 0, EXT2_BLOCK_SIZE);
    memcpy(be->data, target, (uint32_t)targetlen);
    bcache_mark_dirty(be);
    bcache_release(be);

    ip->dirty = 1;
    icache_writeback(ip);
    icache_release(ip);

    /* Add directory entry */
    int ret = ext2_dir_add(parent, basename, (uint32_t)ino, EXT2_FT_SYMLINK);
    icache_writeback(parent);
    icache_release(parent);

    if (ret < 0) {
        ext2_free_inode((uint32_t)ino);
        return ret;
    }

    bcache_sync();
    return 0;
}

static int ext2_readlink(const char *path, char *buf, int bufsiz) {
    if (!ext2_mounted)
        return -1;

    while (*path == '/') path++;

    struct ext2_inode_mem *ip = ext2_namei(path);
    if (!ip)
        return -2; /* ENOENT */

    if (ip->type != EXT2_TYPE_SYMLINK) {
        icache_release(ip);
        return -22; /* EINVAL */
    }

    int copylen = (int)ip->size;
    if (copylen > bufsiz)
        copylen = bufsiz;

    uint32_t blk = ext2_bmap(ip, 0);
    if (blk == 0) {
        icache_release(ip);
        return -1;
    }

    struct bcache_entry *be = bcache_get(blk);
    memcpy(buf, be->data, (uint32_t)copylen);
    bcache_release(be);
    icache_release(ip);
    return copylen;
}

static int ext2_rmdir(const char *path) {
  if (!ext2_mounted)
    return -1;

  while (*path == '/') path++;

  char parent_path[256];
  const char *basename = path_parent(path, parent_path, sizeof(parent_path));
  if (basename[0] == '\0')
    return -1;

  struct ext2_inode_mem *parent = ext2_namei(parent_path);
  if (!parent)
    return -2; /* -ENOENT */
  if (parent->type != EXT2_TYPE_DIR) {
    icache_release(parent);
    return -20; /* -ENOTDIR */
  }

  uint32_t target_ino;
  if (ext2_dir_lookup(parent, basename, &target_ino) < 0) {
    icache_release(parent);
    return -2; /* -ENOENT */
  }

  if (target_ino == EXT2_ROOT_INO) {
    icache_release(parent);
    return -1;
  }

  struct ext2_inode_mem *ip = icache_get(target_ino);
  if (!ip) {
    icache_release(parent);
    return -1;
  }

  if (ip->type != EXT2_TYPE_DIR) {
    icache_release(ip);
    icache_release(parent);
    return -20; /* -ENOTDIR */
  }

  /* Check directory is empty (only . and ..) */
  uint32_t nblocks = (ip->size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;
  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t disk_blk = ext2_bmap(ip, b);
    if (disk_blk == 0) continue;
    struct bcache_entry *e = bcache_get(disk_blk);
    if (!e) continue;
    uint32_t off = 0;
    while (off < EXT2_BLOCK_SIZE) {
      struct ext2_dirent *de = (struct ext2_dirent *)(e->data + off);
      if (de->rec_len == 0) break;
      if (de->inode_number != 0) {
        int is_dot = (de->name_len == 1 && de->name[0] == '.');
        int is_dotdot = (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.');
        if (!is_dot && !is_dotdot) {
          bcache_release(e);
          icache_release(ip);
          icache_release(parent);
          return -39; /* -ENOTEMPTY */
        }
      }
      off += de->rec_len;
    }
    bcache_release(e);
  }

  /* Remove directory entry from parent */
  int ret = ext2_dir_remove(parent, basename);
  icache_writeback(parent);
  icache_release(parent);

  if (ret < 0) {
    icache_release(ip);
    return ret;
  }

  /* Free the directory inode */
  ip->link_count = 0;
  ip->dirty = 1;
  icache_release(ip);
  ext2_free_inode(target_ino);

  bcache_sync();
  sb_flush_if_dirty();
  return 0;
}

/* ================================================================== */
/* VFS ops struct                                                      */
/* ================================================================== */

struct vfs_ops ext2_ops = {
  .open     = ext2_open,
  .read     = ext2_read,
  .write    = ext2_write,
  .close    = ext2_close,
  .stat     = ext2_stat,
  .fstat    = ext2_fstat,
  .readdir  = ext2_readdir,
  .mkdir    = ext2_mkdir,
  .unlink   = ext2_unlink,
  .rmdir    = ext2_rmdir,
  .symlink  = ext2_symlink,
  .readlink = ext2_readlink,
  .rename   = 0,
};

/* ================================================================== */
/* mkfs -- format disk with ext2-lite filesystem                       */
/* ================================================================== */

static int ext2_mkfs(uint64_t capacity_sectors) {
  uint32_t total_blocks = (uint32_t)(capacity_sectors / EXT2_SECTORS_PER_BLOCK);
  if (total_blocks < 16) {
    printk("ext2: disk too small (%d blocks)\n", total_blocks);
    return -1;
  }

  uint32_t total_inodes = total_blocks / 4;
  if (total_inodes > 4096) total_inodes = 4096;
  if (total_inodes < 16)   total_inodes = 16;

  uint32_t bitmap_blocks = (total_blocks + 32767) / 32768;
  uint32_t itable_blocks =
    (total_inodes * EXT2_INODE_SIZE + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;
  uint32_t data_start = 1 + bitmap_blocks + itable_blocks;

  printk("ext2_mkfs: formatting disk...\n");
  printk("  total_blocks=%d total_inodes=%d\n", total_blocks, total_inodes);
  printk("  bitmap_blocks=%d itable_blocks=%d data_start=%d\n",
         bitmap_blocks, itable_blocks, data_start);

  /* Use a static buffer for formatting */
  static uint8_t fmt_buf[EXT2_BLOCK_SIZE]
    __attribute__((aligned(4096)));

  /* Write superblock (block 0) */
  memset(fmt_buf, 0, EXT2_BLOCK_SIZE);
  struct ext2_superblock *fmtsb = (struct ext2_superblock *)fmt_buf;
  fmtsb->magic        = EXT2_MAGIC;
  fmtsb->block_size   = EXT2_BLOCK_SIZE;
  fmtsb->total_blocks = total_blocks;
  fmtsb->total_inodes = total_inodes;
  /* free_blocks: total - metadata - 1 (root dir data block) */
  fmtsb->free_blocks  = total_blocks - data_start - 1;
  /* free_inodes: total - 1 (root inode) */
  fmtsb->free_inodes  = total_inodes - 1;
  fmtsb->bitmap_start = 1;
  fmtsb->bitmap_blocks = bitmap_blocks;
  fmtsb->itable_start = 1 + bitmap_blocks;
  fmtsb->itable_blocks = itable_blocks;
  fmtsb->data_start   = data_start;
  fmtsb->root_inode   = EXT2_ROOT_INO;
  fmtsb->version      = EXT2_VERSION;
  if (ext2_write_block(0, fmt_buf) < 0)
    return -1;

  /* Write block bitmap (mark blocks 0..data_start allocated,
   * plus data_start itself for root dir data block) */
  for (uint32_t bm = 0; bm < bitmap_blocks; bm++) {
    memset(fmt_buf, 0, EXT2_BLOCK_SIZE);
    if (bm == 0) {
      /* Mark blocks 0..data_start as used */
      uint32_t mark_end = data_start + 1; /* +1 for root dir data block */
      for (uint32_t i = 0; i < mark_end; i++) {
        uint32_t byte = i / 8;
        uint32_t bit  = i % 8;
        if (byte < EXT2_BLOCK_SIZE)
          fmt_buf[byte] |= (1 << bit);
      }
    }
    if (ext2_write_block(1 + bm, fmt_buf) < 0)
      return -1;
  }

  /* Zero all inode table blocks */
  memset(fmt_buf, 0, EXT2_BLOCK_SIZE);
  for (uint32_t i = 0; i < itable_blocks; i++) {
    if (ext2_write_block(1 + bitmap_blocks + i, fmt_buf) < 0)
      return -1;
  }

  /* Initialize root directory inode (inode 0) */
  struct ext2_dinode root_di;
  memset(&root_di, 0, sizeof(root_di));
  root_di.type = EXT2_TYPE_DIR;
  root_di.link_count = 1;
  root_di.size = EXT2_BLOCK_SIZE;
  root_di.direct[0] = data_start;
  root_di.mode = 0755;

  /* Write root inode into inode table block */
  memset(fmt_buf, 0, EXT2_BLOCK_SIZE);
  memcpy(fmt_buf, &root_di, sizeof(root_di));
  if (ext2_write_block(1 + bitmap_blocks, fmt_buf) < 0)
    return -1;

  /* Write root directory data block with . and .. entries */
  memset(fmt_buf, 0, EXT2_BLOCK_SIZE);
  struct ext2_dirent *de = (struct ext2_dirent *)fmt_buf;
  de->inode_number = EXT2_ROOT_INO;
  de->rec_len = 12;
  de->name_len = 1;
  de->file_type = EXT2_FT_DIR;
  de->name[0] = '.';

  de = (struct ext2_dirent *)(fmt_buf + 12);
  de->inode_number = EXT2_ROOT_INO;
  de->rec_len = (uint16_t)(EXT2_BLOCK_SIZE - 12);
  de->name_len = 2;
  de->file_type = EXT2_FT_DIR;
  de->name[0] = '.';
  de->name[1] = '.';

  if (ext2_write_block(data_start, fmt_buf) < 0)
    return -1;

  printk("ext2_mkfs: done\n");
  return 0;
}

/* ================================================================== */
/* ext2_init -- called at boot                                         */
/* ================================================================== */

int ext2_init(void) {
  memset(bcache, 0, sizeof(bcache));
  memset(icache, 0, sizeof(icache));
  ext2_mounted = 0;

  /* Try to read superblock from disk */
  static uint8_t init_buf[EXT2_BLOCK_SIZE]
    __attribute__((aligned(4096)));

  if (ext2_read_block(0, init_buf) < 0) {
    printk("ext2: cannot read block 0 (no disk?)\n");
    return -1;
  }

  struct ext2_superblock *disk_sb = (struct ext2_superblock *)init_buf;
  if (disk_sb->magic != EXT2_MAGIC) {
    printk("ext2: no valid filesystem (magic=0x%x, expected 0x%x)\n",
           disk_sb->magic, EXT2_MAGIC);
    printk("ext2: formatting disk...\n");

    /* Need to know disk capacity. Read it from virtio_blk_rw by
     * checking the capacity. We'll use a simple approach:
     * try writing/reading block 0 to estimate. For now, assume
     * the QEMU disk size. We can get capacity from the blk device. */
    uint64_t cap = virtio_blk_capacity();
    if (cap == 0) cap = 131072; /* 64 MB fallback */
    if (ext2_mkfs(cap) < 0) {
      printk("ext2: mkfs failed\n");
      return -1;
    }

    /* Re-read superblock after format */
    if (ext2_read_block(0, init_buf) < 0)
      return -1;
    disk_sb = (struct ext2_superblock *)init_buf;
    if (disk_sb->magic != EXT2_MAGIC) {
      printk("ext2: mkfs produced invalid superblock\n");
      return -1;
    }
  }

  /* Load superblock into memory */
  memcpy(&sb, disk_sb, sizeof(sb));
  sb.dirty = 0;
  ext2_mounted = 1;

  printk("ext2: mounted (%d blocks, %d inodes, %d free blocks, "
         "%d free inodes)\n",
         sb.total_blocks, sb.total_inodes, sb.free_blocks, sb.free_inodes);
  return 0;
}

/* ================================================================== */
/* ext2_sync -- flush all dirty data to disk                           */
/* ================================================================== */

int ext2_mounted_flag(void) {
  return ext2_mounted;
}

void ext2_sync(void) {
  if (!ext2_mounted) return;

  /* Write back all dirty icache entries */
  for (int i = 0; i < ICACHE_SIZE; i++) {
    if (icache[i].valid && icache[i].dirty)
      icache_writeback(&icache[i]);
  }

  /* Write back all dirty bcache entries */
  bcache_sync();

  /* Write superblock if dirty */
  sb_flush_if_dirty();
}
