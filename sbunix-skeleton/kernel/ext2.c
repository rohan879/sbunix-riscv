#include <ext2.h>
#include <vfs.h>
#include <virtio_blk.h>
#include <kalloc.h>
#include <kmalloc.h>
#include <swap.h>
#include <string.h>
#include <printk.h>

/* ================================================================== */
/* Globals                                                             */
/* ================================================================== */
#define EXT2_MAX_FS 2

static struct ext2_fs ext2_filesystems[EXT2_MAX_FS] = {
  { .device_index = 0, .mount_path = "/mnt" },
  { .device_index = 1, .mount_path = "/test" },
};

#define sb          (fs->sb)
#define bcache      (fs->bcache)
#define icache      (fs->icache)
#define ext2_mounted (fs->mounted)
#define bcache_hand (fs->bcache_hand)

/* ================================================================== */
/* Block I/O (low-level)                                               */
/* ================================================================== */

static uint64_t ext2_usable_sectors(struct ext2_fs *fs) {
  uint64_t cap = virtio_blk_capacity_on_dev(fs->device_index);
  if (cap == 0)
    return 0;
  /* Device 0 shares media with swap: reserve swap tail sectors. */
  if (fs->device_index == 0 && cap > SWAP_NSECTORS)
    return cap - SWAP_NSECTORS;
  return cap;
}

static uint32_t ext2_usable_blocks(struct ext2_fs *fs) {
  uint64_t sectors = ext2_usable_sectors(fs);
  return (uint32_t)(sectors / EXT2_SECTORS_PER_BLOCK);
}

static int ext2_read_block(struct ext2_fs *fs, uint32_t blockno, void *buf) {
  uint32_t max_blocks = ext2_usable_blocks(fs);
  if (max_blocks && blockno >= max_blocks)
    return -1;
  return virtio_blk_rw_on_dev(fs->device_index,
                              (uint64_t)blockno * EXT2_SECTORS_PER_BLOCK,
                              buf, 0);
}

static int ext2_write_block(struct ext2_fs *fs, uint32_t blockno,
                            const void *buf) {
  uint32_t max_blocks = ext2_usable_blocks(fs);
  if (max_blocks && blockno >= max_blocks)
    return -1;
  return virtio_blk_rw_on_dev(fs->device_index,
                              (uint64_t)blockno * EXT2_SECTORS_PER_BLOCK,
                              (void *)buf, 1);
}

/* ================================================================== */
/* Block Cache                                                         */
/* ================================================================== */

static struct bcache_entry *bcache_get(struct ext2_fs *fs, uint32_t blockno) {
  /* Check if already cached */
  for (int i = 0; i < BCACHE_SIZE; i++) {
    if (bcache[i].valid && bcache[i].blockno == blockno) {
      bcache[i].refcount++;
      return &bcache[i];
    }
  }

  /* Find a slot to evict using simple clock algorithm.
   * bcache_hand is the clock hand that advances on each examined slot.
   * We prefer clean entries but will write back dirty entries before
   * reusing them. The refcount field pins entries in use. */
  for (int tries = 0; tries < BCACHE_SIZE * 2; tries++) {
    struct bcache_entry *e = &bcache[bcache_hand];
    bcache_hand = (bcache_hand + 1) % BCACHE_SIZE;

    if (e->refcount > 0)
      continue;

    /* Prefer clean entries */
    if (e->valid && e->dirty) {
      /* Write back dirty entry */
      ext2_write_block(fs, e->blockno, e->data);
      e->dirty = 0;
    }

    /* Evict this slot */
    e->blockno = blockno;
    e->valid = 0;
    e->dirty = 0;
    e->refcount = 1;

    /* Read from disk */
    if (ext2_read_block(fs, blockno, e->data) < 0) {
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

static void bcache_sync(struct ext2_fs *fs) {
  for (int i = 0; i < BCACHE_SIZE; i++) {
    if (bcache[i].valid && bcache[i].dirty) {
      ext2_write_block(fs, bcache[i].blockno, bcache[i].data);
      bcache[i].dirty = 0;
    }
  }
}

/* ================================================================== */
/* Superblock helpers                                                  */
/* ================================================================== */

static int sb_write(struct ext2_fs *fs) {
  struct bcache_entry *e = bcache_get(fs, 0);
  if (!e) return -1;
  memcpy(e->data, &sb, 52); /* only copy the on-disk portion */
  memset(e->data + 52, 0, EXT2_BLOCK_SIZE - 52);
  bcache_mark_dirty(e);
  bcache_release(e);
  sb.dirty = 0;
  return 0;
}

static void sb_flush_if_dirty(struct ext2_fs *fs) {
  if (sb.dirty)
    sb_write(fs);
}

/* ================================================================== */
/* Block Bitmap                                                        */
/* ================================================================== */

static int ext2_alloc_block(struct ext2_fs *fs) {
  /* Scan bitmap for a free bit starting from data_start */
  for (uint32_t bm_block = 0; bm_block < sb.bitmap_blocks; bm_block++) {
    struct bcache_entry *e = bcache_get(fs, sb.bitmap_start + bm_block);
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
          sb_flush_if_dirty(fs);
          return (int)blockno;
        }
      }
    }
    bcache_release(e);
  }
  return -1; /* disk full */
}

static void ext2_free_block(struct ext2_fs *fs, uint32_t blockno) {
  uint32_t bm_block = blockno / (EXT2_BLOCK_SIZE * 8);
  uint32_t bit_idx  = blockno % (EXT2_BLOCK_SIZE * 8);
  uint32_t byte_idx = bit_idx / 8;
  uint32_t bit_off  = bit_idx % 8;

  struct bcache_entry *e = bcache_get(fs, sb.bitmap_start + bm_block);
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

static int ext2_read_dinode(struct ext2_fs *fs, uint32_t ino,
                            struct ext2_dinode *di) {
  uint32_t block = sb.itable_start + (ino * EXT2_INODE_SIZE) / EXT2_BLOCK_SIZE;
  uint32_t off   = (ino * EXT2_INODE_SIZE) % EXT2_BLOCK_SIZE;

  struct bcache_entry *e = bcache_get(fs, block);
  if (!e) return -1;
  memcpy(di, e->data + off, sizeof(struct ext2_dinode));
  bcache_release(e);
  return 0;
}

static int ext2_write_dinode(struct ext2_fs *fs, uint32_t ino,
                             struct ext2_dinode *di) {
  uint32_t block = sb.itable_start + (ino * EXT2_INODE_SIZE) / EXT2_BLOCK_SIZE;
  uint32_t off   = (ino * EXT2_INODE_SIZE) % EXT2_BLOCK_SIZE;

  struct bcache_entry *e = bcache_get(fs, block);
  if (!e) return -1;
  memcpy(e->data + off, di, sizeof(struct ext2_dinode));
  bcache_mark_dirty(e);
  bcache_release(e);
  return 0;
}

/* ================================================================== */
/* Inode Cache                                                         */
/* ================================================================== */

static struct ext2_inode_mem *icache_get(struct ext2_fs *fs, uint32_t ino) {
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
          ext2_write_dinode(fs, icache[i].ino, &di);
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
  if (ext2_read_dinode(fs, ino, &di) < 0)
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

static void icache_writeback(struct ext2_fs *fs, struct ext2_inode_mem *ip) {
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
  /* BF: Preserve timestamps on writeback -- the eviction path (lines 273-274)
   * correctly copies these, but icache_writeback was omitting them, causing
   * on-disk timestamps to be zeroed on every explicit writeback (write, close,
   * fsync).  This broke POSIX mmap atime consistency tests. */
  di.ctime = ip->ctime;
  di.mtime = ip->mtime;
  ext2_write_dinode(fs, ip->ino, &di);
  ip->dirty = 0;
}

static void icache_evict(struct ext2_fs *fs, uint32_t ino) {
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

static int ext2_alloc_inode(struct ext2_fs *fs, uint16_t type) {
  for (uint32_t ino = 0; ino < sb.total_inodes; ino++) {
    struct ext2_dinode di;
    if (ext2_read_dinode(fs, ino, &di) < 0)
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
      ext2_write_dinode(fs, ino, &di);
      sb.free_inodes--;
      sb.dirty = 1;
      sb_flush_if_dirty(fs);
      return (int)ino;
    }
  }
  return -1;
}

static void ext2_free_inode(struct ext2_fs *fs, uint32_t ino) {
  struct ext2_inode_mem *ip = icache_get(fs, ino);
  if (!ip) {
    /* Read directly */
    struct ext2_dinode di;
    if (ext2_read_dinode(fs, ino, &di) < 0)
      return;
    /* Free direct blocks */
    for (int i = 0; i < EXT2_NDIRECT; i++) {
      if (di.direct[i])
        ext2_free_block(fs, di.direct[i]);
    }
    /* Free indirect blocks */
    if (di.indirect) {
      struct bcache_entry *e = bcache_get(fs, di.indirect);
      if (e) {
        uint32_t *ptrs = (uint32_t *)e->data;
        for (int i = 0; i < EXT2_NINDIRECT; i++) {
          if (ptrs[i])
            ext2_free_block(fs, ptrs[i]);
        }
        bcache_release(e);
      }
      ext2_free_block(fs, di.indirect);
    }
    /* Zero the inode */
    memset(&di, 0, sizeof(di));
    ext2_write_dinode(fs, ino, &di);
  } else {
    /* Free direct blocks */
    for (int i = 0; i < EXT2_NDIRECT; i++) {
      if (ip->direct[i])
        ext2_free_block(fs, ip->direct[i]);
    }
    /* Free indirect blocks */
    if (ip->indirect) {
      struct bcache_entry *e = bcache_get(fs, ip->indirect);
      if (e) {
        uint32_t *ptrs = (uint32_t *)e->data;
        for (int i = 0; i < EXT2_NINDIRECT; i++) {
          if (ptrs[i])
            ext2_free_block(fs, ptrs[i]);
        }
        bcache_release(e);
      }
      ext2_free_block(fs, ip->indirect);
    }
    icache_release(ip);
    icache_evict(fs, ino);
    /* Zero the inode on disk */
    struct ext2_dinode di;
    memset(&di, 0, sizeof(di));
    ext2_write_dinode(fs, ino, &di);
  }
  sb.free_inodes++;
  sb.dirty = 1;
}

/* ================================================================== */
/* Block Mapping (bmap)                                                */
/* ================================================================== */

static uint32_t ext2_bmap(struct ext2_fs *fs, struct ext2_inode_mem *ip,
                          uint32_t file_block) {
  if (file_block < EXT2_NDIRECT) {
    return ip->direct[file_block];
  }
  uint32_t idx = file_block - EXT2_NDIRECT;
  if (idx >= EXT2_NINDIRECT)
    return 0;
  if (ip->indirect == 0)
    return 0;
  struct bcache_entry *e = bcache_get(fs, ip->indirect);
  if (!e) return 0;
  uint32_t *ptrs = (uint32_t *)e->data;
  uint32_t blk = ptrs[idx];
  bcache_release(e);
  return blk;
}

/* bmap_alloc: like bmap but allocates blocks on demand.
 * Returns disk block number, or 0 on failure. */
static uint32_t ext2_bmap_alloc(struct ext2_fs *fs, struct ext2_inode_mem *ip,
                                uint32_t file_block) {
  if (file_block < EXT2_NDIRECT) {
    if (ip->direct[file_block] == 0) {
      int blk = ext2_alloc_block(fs);
      if (blk < 0) return 0;
      ip->direct[file_block] = (uint32_t)blk;
      ip->dirty = 1;
      /* Zero the new block */
      struct bcache_entry *e = bcache_get(fs, (uint32_t)blk);
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
    int blk = ext2_alloc_block(fs);
    if (blk < 0) return 0;
    ip->indirect = (uint32_t)blk;
    ip->dirty = 1;
    /* Zero the indirect block */
    struct bcache_entry *e = bcache_get(fs, (uint32_t)blk);
    if (e) {
      memset(e->data, 0, EXT2_BLOCK_SIZE);
      bcache_mark_dirty(e);
      bcache_release(e);
    }
  }

  struct bcache_entry *e = bcache_get(fs, ip->indirect);
  if (!e) return 0;
  uint32_t *ptrs = (uint32_t *)e->data;
  if (ptrs[idx] == 0) {
    int blk = ext2_alloc_block(fs);
    if (blk < 0) {
      bcache_release(e);
      return 0;
    }
    ptrs[idx] = (uint32_t)blk;
    bcache_mark_dirty(e);
    /* Zero the new data block */
    struct bcache_entry *e2 = bcache_get(fs, (uint32_t)blk);
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

static int ext2_dir_lookup(struct ext2_fs *fs, struct ext2_inode_mem *dir,
                           const char *name,
                           uint32_t *out_ino) {
  if (dir->type != EXT2_TYPE_DIR)
    return -1;

  uint32_t name_len = strlen(name);
  uint32_t nblocks = (dir->size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t disk_blk = ext2_bmap(fs, dir, b);
    if (disk_blk == 0) continue;

    struct bcache_entry *e = bcache_get(fs, disk_blk);
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

static int ext2_dir_add(struct ext2_fs *fs, struct ext2_inode_mem *dir,
                        const char *name,
                        uint32_t ino, uint8_t file_type) {
  uint32_t name_len = strlen(name);
  uint32_t needed = ext2_dirent_size(name_len);
  uint32_t nblocks = (dir->size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

  /* Try to find space in existing blocks */
  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t disk_blk = ext2_bmap(fs, dir, b);
    if (disk_blk == 0) continue;

    struct bcache_entry *e = bcache_get(fs, disk_blk);
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
  uint32_t new_blk = ext2_bmap_alloc(fs, dir, nblocks);
  if (new_blk == 0)
    return -28; /* -ENOSPC */

  struct bcache_entry *e = bcache_get(fs, new_blk);
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

static int ext2_dir_remove(struct ext2_fs *fs, struct ext2_inode_mem *dir,
                           const char *name) {
  uint32_t name_len = strlen(name);
  uint32_t nblocks = (dir->size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t disk_blk = ext2_bmap(fs, dir, b);
    if (disk_blk == 0) continue;

    struct bcache_entry *e = bcache_get(fs, disk_blk);
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

/* Resolve path relative to ext2 root (with symlink following).
 * Returns inode with refcount++ on success.
 * The path is relative to the /mnt mount point (VFS strips prefix). */
static struct ext2_inode_mem *ext2_namei_r(struct ext2_fs *fs, const char *path,
                                           int symdepth) {
  if (symdepth <= 0) return NULL; /* ELOOP */

  /* Start from root inode */
  struct ext2_inode_mem *ip = icache_get(fs, EXT2_ROOT_INO);
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
    if (ext2_dir_lookup(fs, ip, comp, &child_ino) < 0) {
      icache_release(ip);
      return NULL;
    }

    icache_release(ip);
    ip = icache_get(fs, child_ino);
    if (!ip) return NULL;

    /* Skip trailing slashes */
    while (*p == '/') p++;

    /* Follow symlinks in intermediate path components */
    if (ip->type == EXT2_TYPE_SYMLINK && *p != '\0') {
      char target[256];
      int tlen = (int)ip->size;
      if (tlen > 255) tlen = 255;
      uint32_t blk = ext2_bmap(fs, ip, 0);
      if (blk == 0) { icache_release(ip); return NULL; }
      struct bcache_entry *be = bcache_get(fs, blk);
      memcpy(target, be->data, (uint32_t)tlen);
      bcache_release(be);
      target[tlen] = '\0';
      icache_release(ip);

      /* Build new path: prefix + target + "/" + remaining */
      char newpath[512];
      int prefix_len = (int)(start - path);
      int remaining_len = (int)strlen(p);
      if (prefix_len + tlen + 1 + remaining_len >= 510)
        return NULL;
      int npos = 0;
      if (prefix_len > 0) {
        memcpy(newpath, path, prefix_len);
        npos = prefix_len;
      }
      memcpy(newpath + npos, target, tlen);
      npos += tlen;
      if (remaining_len > 0) {
        newpath[npos] = '/';
        memcpy(newpath + npos + 1, p, remaining_len + 1);
      } else {
        newpath[npos] = '\0';
      }

      return ext2_namei_r(fs, newpath, symdepth - 1);
    }
  }

  return ip;
}

static struct ext2_inode_mem *ext2_namei(struct ext2_fs *fs, const char *path) {
  return ext2_namei_r(fs, path, 8);
}

/* ================================================================== */
/* Truncate -- free all data blocks of an inode                        */
/* ================================================================== */

static void ext2_truncate(struct ext2_fs *fs, struct ext2_inode_mem *ip) {
  /* Free direct blocks */
  for (int i = 0; i < EXT2_NDIRECT; i++) {
    if (ip->direct[i]) {
      ext2_free_block(fs, ip->direct[i]);
      ip->direct[i] = 0;
    }
  }
  /* Free indirect blocks */
  if (ip->indirect) {
    struct bcache_entry *e = bcache_get(fs, ip->indirect);
    if (e) {
      uint32_t *ptrs = (uint32_t *)e->data;
      for (int i = 0; i < EXT2_NINDIRECT; i++) {
        if (ptrs[i])
          ext2_free_block(fs, ptrs[i]);
      }
      bcache_release(e);
    }
    ext2_free_block(fs, ip->indirect);
    ip->indirect = 0;
  }
  ip->size = 0;
  ip->dirty = 1;
}

/* ================================================================== */
/* VFS Ops Implementation                                              */
/* ================================================================== */

static int ext2_open(void *fs_data, const char *path, int flags,
                     void **private_out) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  if (!fs || !ext2_mounted)
    return -1;

  /* Strip leading slashes for namei */
  while (*path == '/') path++;

  struct ext2_inode_mem *ip = ext2_namei(fs, path);

  if (!ip && (flags & EXT2_O_CREAT)) {
    /* Create a new file */
    /* Find parent directory */
    /* BF: Reduced from 256 to 128 to prevent kernel stack overflow.
     * Path components > 128 chars are rare; kernel paths are short. */
    char parent_path[128];
    const char *basename = path_parent(path, parent_path, sizeof(parent_path));
    if (basename[0] == '\0')
      return -1;

    struct ext2_inode_mem *parent = ext2_namei(fs, parent_path);
    if (!parent)
      return -2; /* -ENOENT */
    if (parent->type != EXT2_TYPE_DIR) {
      icache_release(parent);
      return -20; /* -ENOTDIR */
    }

    /* Allocate new inode */
    int new_ino = ext2_alloc_inode(fs, EXT2_TYPE_FILE);
    if (new_ino < 0) {
      icache_release(parent);
      return -28; /* -ENOSPC */
    }

    /* Add directory entry */
    int ret = ext2_dir_add(fs, parent, basename, (uint32_t)new_ino,
                           EXT2_FT_FILE);
    icache_writeback(fs, parent);
    icache_release(parent);
    if (ret < 0) {
      ext2_free_inode(fs, (uint32_t)new_ino);
      return ret;
    }

    /* Flush all dirty bcache blocks so the new dir entry is visible on disk */
    bcache_sync(fs);

    ip = icache_get(fs, (uint32_t)new_ino);
    if (!ip) return -1;
  }

  if (!ip)
    return -2; /* -ENOENT */

  /* Handle O_TRUNC */
  if ((flags & EXT2_O_TRUNC) && ip->type == EXT2_TYPE_FILE) {
    ext2_truncate(fs, ip);
    icache_writeback(fs, ip);
  }

  /* Allocate private data */
  struct ext2_open_private *priv =
    (struct ext2_open_private *)kmalloc(sizeof(struct ext2_open_private));
  if (!priv) {
    icache_release(ip);
    return -12; /* -ENOMEM */
  }
  priv->fs = fs;
  priv->ino = ip->ino;
  priv->is_dir = (ip->type == EXT2_TYPE_DIR);
  /* Keep refcount held from icache_get */

  *private_out = priv;
  return 0;
}

static int64_t ext2_read(void *fs_data, void *private, char *buf,
                         uint64_t count, int64_t *offset) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv) return -1;

  /* Reading a directory fd is not allowed */
  if (priv->is_dir) return -21; /* -EISDIR */

  struct ext2_inode_mem *ip = icache_get(fs, priv->ino);
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
    uint32_t disk_blk = ext2_bmap(fs, ip, file_block);

    uint64_t chunk = EXT2_BLOCK_SIZE - block_off;
    if (chunk > count - total) chunk = count - total;

    if (disk_blk == 0) {
      /* Hole: zero-fill */
      memset(buf + total, 0, chunk);
    } else {
      struct bcache_entry *e = bcache_get(fs, disk_blk);
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

static int64_t ext2_write(void *fs_data, void *private, const char *buf,
                          uint64_t count, int64_t *offset) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv) return -1;

  struct ext2_inode_mem *ip = icache_get(fs, priv->ino);
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

    uint32_t disk_blk = ext2_bmap_alloc(fs, ip, file_block);
    if (disk_blk == 0) {
      if (total == 0) {
        icache_release(ip);
        return -28; /* -ENOSPC */
      }
      break;
    }

    struct bcache_entry *e = bcache_get(fs, disk_blk);
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
  icache_writeback(fs, ip);
  icache_release(ip);
  /* Flush all dirty blocks (data + inode) to disk so contents survive reboot */
  bcache_sync(fs);
  return (int64_t)total;
}

static int ext2_ftruncate(void *fs_data, void *private, uint64_t length) {
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv)
    return -9; /* EBADF */
  return ext2_ftruncate_ino(fs_data, priv->ino, length);
}

static int ext2_close(void *fs_data, void *private) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv) return 0;

  struct ext2_inode_mem *ip = icache_get(fs, priv->ino);
  if (ip) {
    /* We got a new ref from icache_get, plus there was one from open.
     * Release both: the open-time ref and this lookup ref. */
    icache_release(ip); /* release our lookup ref */
    icache_release(ip); /* release the open-time ref */

    if (ip->refcount == 0 && ip->link_count == 0 &&
        ip->ino != EXT2_ROOT_INO) {
      ext2_free_inode(fs, ip->ino);
    } else {
      icache_writeback(fs, ip);
      /* Flush inode writeback to disk */
      bcache_sync(fs);
    }
  }

  kmfree(priv, sizeof(struct ext2_open_private));
  return 0;
}

static int ext2_stat(void *fs_data, const char *path, uint8_t *st) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  if (!fs || !ext2_mounted)
    return -1;

  while (*path == '/') path++;

  struct ext2_inode_mem *ip = ext2_namei(fs, path);
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

static int ext2_fstat(void *fs_data, void *private, uint8_t *st) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv) return -1;

  struct ext2_inode_mem *ip = icache_get(fs, priv->ino);
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

static int ext2_readdir(void *fs_data, void *private, char *ubuf,
                        unsigned int count, int64_t *offset) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  struct ext2_open_private *priv = (struct ext2_open_private *)private;
  if (!priv || !priv->is_dir)
    return -1;

  struct ext2_inode_mem *ip = icache_get(fs, priv->ino);
  if (!ip) return -1;

  uint64_t dir_off = (uint64_t)*offset;
  unsigned int written = 0;
  uint64_t pos = 0;
  uint32_t nblocks = (ip->size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t disk_blk = ext2_bmap(fs, ip, b);
    if (disk_blk == 0) {
      pos += EXT2_BLOCK_SIZE;
      continue;
    }

    struct bcache_entry *e = bcache_get(fs, disk_blk);
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

static int ext2_mkdir(void *fs_data, const char *path, int mode) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  if (!fs || !ext2_mounted)
    return -1;

  while (*path == '/') path++;

  /* Parse parent and basename */
  char parent_path[256];
  const char *basename = path_parent(path, parent_path, sizeof(parent_path));
  if (basename[0] == '\0')
    return -1;

  struct ext2_inode_mem *parent = ext2_namei(fs, parent_path);
  if (!parent)
    return -2; /* -ENOENT */
  if (parent->type != EXT2_TYPE_DIR) {
    icache_release(parent);
    return -20; /* -ENOTDIR */
  }

  /* Check that name doesn't exist */
  uint32_t existing;
  if (ext2_dir_lookup(fs, parent, basename, &existing) == 0) {
    icache_release(parent);
    return -17; /* -EEXIST */
  }

  /* Allocate new inode for the directory */
  int new_ino = ext2_alloc_inode(fs, EXT2_TYPE_DIR);
  if (new_ino < 0) {
    icache_release(parent);
    return -28; /* -ENOSPC */
  }

  /* Get the new inode into icache */
  struct ext2_inode_mem *ip = icache_get(fs, (uint32_t)new_ino);
  if (!ip) {
    ext2_free_inode(fs, (uint32_t)new_ino);
    icache_release(parent);
    return -1;
  }

  /* Set mode */
  ip->mode = (uint32_t)(mode & 0xfff);
  if (ip->mode == 0)
    ip->mode = 0755;

  /* Allocate first data block for . and .. entries */
  uint32_t data_blk = ext2_bmap_alloc(fs, ip, 0);
  if (data_blk == 0) {
    icache_release(ip);
    ext2_free_inode(fs, (uint32_t)new_ino);
    icache_release(parent);
    return -28; /* -ENOSPC */
  }

  ip->size = EXT2_BLOCK_SIZE;
  ip->dirty = 1;

  /* Write . and .. entries */
  struct bcache_entry *e = bcache_get(fs, data_blk);
  if (!e) {
    icache_release(ip);
    ext2_free_inode(fs, (uint32_t)new_ino);
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

  icache_writeback(fs, ip);
  icache_release(ip);

  /* Add entry in parent directory */
  int ret = ext2_dir_add(fs, parent, basename, (uint32_t)new_ino,
                         EXT2_FT_DIR);
  icache_writeback(fs, parent);
  icache_release(parent);

  if (ret < 0) {
    ext2_free_inode(fs, (uint32_t)new_ino);
    return ret;
  }

  bcache_sync(fs);
  return 0;
}

static int ext2_unlink(void *fs_data, const char *path) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  if (!fs || !ext2_mounted)
    return -1;

  while (*path == '/') path++;

  /* Parse parent and basename */
  char parent_path[256];
  const char *basename = path_parent(path, parent_path, sizeof(parent_path));
  if (basename[0] == '\0')
    return -1;

  struct ext2_inode_mem *parent = ext2_namei(fs, parent_path);
  if (!parent)
    return -2; /* -ENOENT */
  if (parent->type != EXT2_TYPE_DIR) {
    icache_release(parent);
    return -20; /* -ENOTDIR */
  }

  /* Look up the target inode */
  uint32_t target_ino;
  if (ext2_dir_lookup(fs, parent, basename, &target_ino) < 0) {
    icache_release(parent);
    return -2; /* -ENOENT */
  }

  /* Don't allow unlinking root */
  if (target_ino == EXT2_ROOT_INO) {
    icache_release(parent);
    return -1;
  }

  struct ext2_inode_mem *ip = icache_get(fs, target_ino);
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
  int ret = ext2_dir_remove(fs, parent, basename);
  icache_writeback(fs, parent);
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
    ext2_free_inode(fs, target_ino);
  } else {
    icache_writeback(fs, ip);
    icache_release(ip);
  }

  bcache_sync(fs);
  sb_flush_if_dirty(fs);
  return 0;
}

int ext2_mknod(void *fs_data, const char *path, uint32_t mode, uint32_t rdev) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  if (!fs || !ext2_mounted)
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

  struct ext2_inode_mem *parent = ext2_namei(fs, parent_path);
  if (!parent)
    return -2; /* -ENOENT */
  if (parent->type != EXT2_TYPE_DIR) {
    icache_release(parent);
    return -20; /* -ENOTDIR */
  }

  /* Check name doesn't exist */
  uint32_t existing;
  if (ext2_dir_lookup(fs, parent, basename, &existing) == 0) {
    icache_release(parent);
    return -17; /* -EEXIST */
  }

  int new_ino = ext2_alloc_inode(fs, itype);
  if (new_ino < 0) {
    icache_release(parent);
    return -28; /* -ENOSPC */
  }

  struct ext2_inode_mem *ip = icache_get(fs, (uint32_t)new_ino);
  if (!ip) {
    ext2_free_inode(fs, (uint32_t)new_ino);
    icache_release(parent);
    return -1;
  }

  ip->mode = mode & 0xfff;
  ip->rdev = rdev;
  ip->dirty = 1;
  icache_writeback(fs, ip);
  icache_release(ip);

  int ret = ext2_dir_add(fs, parent, basename, (uint32_t)new_ino, ftype);
  icache_writeback(fs, parent);
  icache_release(parent);

  if (ret < 0) {
    ext2_free_inode(fs, (uint32_t)new_ino);
    return ret;
  }

  bcache_sync(fs);
  return 0;
}

static int ext2_symlink(void *fs_data, const char *target,
                        const char *linkpath) {
    struct ext2_fs *fs = (struct ext2_fs *)fs_data;
    if (!fs || !ext2_mounted)
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

    struct ext2_inode_mem *parent = ext2_namei(fs, parent_path);
    if (!parent)
        return -2; /* ENOENT */

    /* Check name doesn't already exist */
    uint32_t existing_ino;
    if (ext2_dir_lookup(fs, parent, basename, &existing_ino) == 0) {
        icache_release(parent);
        return -17; /* EEXIST */
    }

    /* Allocate inode */
    int ino = ext2_alloc_inode(fs, EXT2_TYPE_SYMLINK);
    if (ino < 0) {
        icache_release(parent);
        return -28; /* ENOSPC */
    }

    struct ext2_inode_mem *ip = icache_get(fs, (uint32_t)ino);
    if (!ip) {
        icache_release(parent);
        return -1;
    }

    ip->mode = 0777;
    ip->size = (uint32_t)targetlen;

    /* Allocate data block and write target */
    uint32_t blk = ext2_bmap_alloc(fs, ip, 0);
    if (blk == 0) {
        icache_release(ip);
        icache_release(parent);
        return -28; /* ENOSPC */
    }

    struct bcache_entry *be = bcache_get(fs, blk);
    memset(be->data, 0, EXT2_BLOCK_SIZE);
    memcpy(be->data, target, (uint32_t)targetlen);
    bcache_mark_dirty(be);
    bcache_release(be);

    ip->dirty = 1;
    icache_writeback(fs, ip);
    icache_release(ip);

    /* Add directory entry */
    int ret = ext2_dir_add(fs, parent, basename, (uint32_t)ino,
                           EXT2_FT_SYMLINK);
    icache_writeback(fs, parent);
    icache_release(parent);

    if (ret < 0) {
        ext2_free_inode(fs, (uint32_t)ino);
        return ret;
    }

    bcache_sync(fs);
    return 0;
}

static int ext2_readlink(void *fs_data, const char *path, char *buf,
                         int bufsiz) {
    struct ext2_fs *fs = (struct ext2_fs *)fs_data;
    if (!fs || !ext2_mounted)
        return -1;

    while (*path == '/') path++;

    struct ext2_inode_mem *ip = ext2_namei(fs, path);
    if (!ip)
        return -2; /* ENOENT */

    if (ip->type != EXT2_TYPE_SYMLINK) {
        icache_release(ip);
        return -22; /* EINVAL */
    }

    int copylen = (int)ip->size;
    if (copylen > bufsiz)
        copylen = bufsiz;

    uint32_t blk = ext2_bmap(fs, ip, 0);
    if (blk == 0) {
        icache_release(ip);
        return -1;
    }

    struct bcache_entry *be = bcache_get(fs, blk);
    memcpy(buf, be->data, (uint32_t)copylen);
    bcache_release(be);
    icache_release(ip);
    return copylen;
}

static int ext2_rename(void *fs_data, const char *oldpath,
                       const char *newpath) {
    struct ext2_fs *fs = (struct ext2_fs *)fs_data;
    if (!fs || !ext2_mounted)
        return -1;

    while (*oldpath == '/') oldpath++;
    while (*newpath == '/') newpath++;

    /* Parse both paths */
    char old_parent_path[256], new_parent_path[256];
    const char *old_basename = path_parent(oldpath, old_parent_path, sizeof(old_parent_path));
    const char *new_basename = path_parent(newpath, new_parent_path, sizeof(new_parent_path));

    if (old_basename[0] == '\0' || new_basename[0] == '\0')
        return -22;

    struct ext2_inode_mem *old_parent = ext2_namei(fs, old_parent_path);
    if (!old_parent)
        return -2;

    /* Look up old entry */
    uint32_t ino;
    if (ext2_dir_lookup(fs, old_parent, old_basename, &ino) < 0) {
        icache_release(old_parent);
        return -2; /* ENOENT */
    }

    /* Get the inode to determine file type */
    struct ext2_inode_mem *ip = icache_get(fs, ino);
    if (!ip) {
        icache_release(old_parent);
        return -1;
    }
    uint8_t ft = (ip->type == EXT2_TYPE_DIR) ? EXT2_FT_DIR :
                 (ip->type == EXT2_TYPE_SYMLINK) ? EXT2_FT_SYMLINK : EXT2_FT_FILE;
    icache_release(ip);

    struct ext2_inode_mem *new_parent = ext2_namei(fs, new_parent_path);
    if (!new_parent) {
        icache_release(old_parent);
        return -2;
    }

    /* If new name exists, remove it */
    uint32_t existing;
    if (ext2_dir_lookup(fs, new_parent, new_basename, &existing) == 0) {
        ext2_dir_remove(fs, new_parent, new_basename);
    }

    /* Add new entry, remove old */
    int ret = ext2_dir_add(fs, new_parent, new_basename, ino, ft);
    if (ret == 0)
        ext2_dir_remove(fs, old_parent, old_basename);

    icache_writeback(fs, new_parent);
    icache_writeback(fs, old_parent);
    icache_release(new_parent);
    icache_release(old_parent);
    bcache_sync(fs);
    return ret;
}

static int ext2_rmdir(void *fs_data, const char *path) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  if (!fs || !ext2_mounted)
    return -1;

  while (*path == '/') path++;

  char parent_path[256];
  const char *basename = path_parent(path, parent_path, sizeof(parent_path));
  if (basename[0] == '\0')
    return -1;

  struct ext2_inode_mem *parent = ext2_namei(fs, parent_path);
  if (!parent)
    return -2; /* -ENOENT */
  if (parent->type != EXT2_TYPE_DIR) {
    icache_release(parent);
    return -20; /* -ENOTDIR */
  }

  uint32_t target_ino;
  if (ext2_dir_lookup(fs, parent, basename, &target_ino) < 0) {
    icache_release(parent);
    return -2; /* -ENOENT */
  }

  if (target_ino == EXT2_ROOT_INO) {
    icache_release(parent);
    return -1;
  }

  struct ext2_inode_mem *ip = icache_get(fs, target_ino);
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
    uint32_t disk_blk = ext2_bmap(fs, ip, b);
    if (disk_blk == 0) continue;
    struct bcache_entry *e = bcache_get(fs, disk_blk);
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
  int ret = ext2_dir_remove(fs, parent, basename);
  icache_writeback(fs, parent);
  icache_release(parent);

  if (ret < 0) {
    icache_release(ip);
    return ret;
  }

  /* Free the directory inode */
  ip->link_count = 0;
  ip->dirty = 1;
  icache_release(ip);
  ext2_free_inode(fs, target_ino);

  bcache_sync(fs);
  sb_flush_if_dirty(fs);
  return 0;
}

/* ================================================================== */
/* VFS ops struct                                                      */
/* ================================================================== */

struct vfs_ops ext2_ops = {
  .open     = ext2_open,
  .read     = ext2_read,
  .write    = ext2_write,
  .ftruncate = ext2_ftruncate,
  .close    = ext2_close,
  .stat     = ext2_stat,
  .fstat    = ext2_fstat,
  .readdir  = ext2_readdir,
  .mkdir    = ext2_mkdir,
  .unlink   = ext2_unlink,
  .rmdir    = ext2_rmdir,
  .symlink  = ext2_symlink,
  .readlink = ext2_readlink,
  .rename   = ext2_rename,
};

/* ================================================================== */
/* mkfs -- format disk with ext2-lite filesystem                       */
/* ================================================================== */

static int ext2_mkfs(struct ext2_fs *fs, uint64_t capacity_sectors) {
  uint32_t total_blocks = (uint32_t)(capacity_sectors / EXT2_SECTORS_PER_BLOCK);
  if (total_blocks < 16) {
    printk("ext2: disk too small (%d blocks)\n", total_blocks);
    return -1;
  }

  uint32_t total_inodes = total_blocks / 4;
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
  if (ext2_write_block(fs, 0, fmt_buf) < 0)
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
    if (ext2_write_block(fs, 1 + bm, fmt_buf) < 0)
      return -1;
  }

  /* Zero all inode table blocks */
  memset(fmt_buf, 0, EXT2_BLOCK_SIZE);
  for (uint32_t i = 0; i < itable_blocks; i++) {
    if (ext2_write_block(fs, 1 + bitmap_blocks + i, fmt_buf) < 0)
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
  if (ext2_write_block(fs, 1 + bitmap_blocks, fmt_buf) < 0)
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

  if (ext2_write_block(fs, data_start, fmt_buf) < 0)
    return -1;

  printk("ext2_mkfs: done\n");
  return 0;
}

/* ================================================================== */
/* ext2_init -- called at boot                                         */
/* ================================================================== */

int ext2_init(void) {
  int mounted = 0;
  static uint8_t init_buf[EXT2_BLOCK_SIZE]
    __attribute__((aligned(4096)));

  /* Probe each configured ext2_filesystems[] entry. For each present
   * virtio block device we attempt to read the superblock. If no
   * valid filesystem is found we run ext2_mkfs to format the device,
   * then mount and register the filesystem state in ext2_filesystems[].
   * This supports multiple independent ext2 mounts (e.g. /mnt and /test).
   */
  for (int i = 0; i < EXT2_MAX_FS; i++) {
    struct ext2_fs *fs = &ext2_filesystems[i];

    memset(bcache, 0, sizeof(bcache));
    memset(icache, 0, sizeof(icache));
    fs->mounted = 0;
    bcache_hand = 0;

    if (fs->device_index >= virtio_blk_count())
      continue;

    if (ext2_read_block(fs, 0, init_buf) < 0) {
      printk("ext2[%d]: cannot read block 0\n", fs->device_index);
      continue;
    }

    struct ext2_superblock *disk_sb = (struct ext2_superblock *)init_buf;
    uint64_t cap = virtio_blk_capacity_on_dev(fs->device_index);
    if (cap == 0) cap = 131072; /* 64 MB fallback */
    uint64_t usable_sectors = cap;
    if (fs->device_index == 0 && cap > SWAP_NSECTORS)
      usable_sectors = cap - SWAP_NSECTORS;
    uint32_t usable_blocks = (uint32_t)(usable_sectors / EXT2_SECTORS_PER_BLOCK);

    int need_format = 0;
    if (disk_sb->magic != EXT2_MAGIC)
      need_format = 1;
    else if (fs->device_index == 0 && usable_blocks > 0 &&
             disk_sb->total_blocks > usable_blocks)
      need_format = 1;

    if (need_format) {
      if (disk_sb->magic != EXT2_MAGIC) {
        printk("ext2[%d]: no valid filesystem (magic=0x%x, expected 0x%x)\n",
               fs->device_index, disk_sb->magic, EXT2_MAGIC);
      } else {
        printk("ext2[%d]: filesystem overlaps swap reservation; reformatting\n",
               fs->device_index);
      }
      if (fs->device_index == 0)
        printk("ext2[%d]: reserving %d sectors for swap\n",
               fs->device_index, SWAP_NSECTORS);
      printk("ext2[%d]: formatting disk...\n", fs->device_index);

      {
        if (ext2_mkfs(fs, usable_sectors) < 0) {
          printk("ext2[%d]: mkfs failed\n", fs->device_index);
          continue;
        }
      }

      if (ext2_read_block(fs, 0, init_buf) < 0)
        continue;
      disk_sb = (struct ext2_superblock *)init_buf;
      if (disk_sb->magic != EXT2_MAGIC) {
        printk("ext2[%d]: mkfs produced invalid superblock\n",
               fs->device_index);
        continue;
      }
    }

    memcpy(&sb, disk_sb, sizeof(sb));
    sb.dirty = 0;
    fs->mounted = 1;
    mounted++;

    printk("ext2[%d]: mounted at %s (%d blocks, %d inodes, %d free blocks, "
           "%d free inodes)\n",
           fs->device_index, fs->mount_path,
           sb.total_blocks, sb.total_inodes, sb.free_blocks, sb.free_inodes);
  }

  return mounted ? 0 : -1;
}

int ext2_register_mounts(void) {
  for (int i = 0; i < EXT2_MAX_FS; i++) {
    struct ext2_fs *fs = &ext2_filesystems[i];
    if (fs->mounted)
      vfs_register_mount(fs->mount_path, &ext2_ops, fs, "ext2");
  }
  return 0;
}

int ext2_is_ops(struct vfs_ops *ops) {
  return ops == &ext2_ops;
}

/* ================================================================== */
/* ext2_sync -- flush all dirty data to disk                           */
/* ================================================================== */

/* ext2_ftruncate_ino -- set file size by inode number */
int ext2_ftruncate_ino(void *fs_data, uint32_t ino, uint64_t length) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  if (!fs || !ext2_mounted) return -30; /* EROFS */
  struct ext2_inode_mem *ip = icache_get(fs, ino);
  if (!ip) return -1;
  ip->size = (uint32_t)length;
  ip->dirty = 1;
  icache_writeback(fs, ip);
  icache_release(ip);
  return 0;
}

/* ext2_fchmod_ino -- set file mode by inode number */
int ext2_fchmod_ino(void *fs_data, uint32_t ino, uint32_t mode) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  if (!fs || !ext2_mounted) return -30; /* EROFS */
  struct ext2_inode_mem *ip = icache_get(fs, ino);
  if (!ip) return -1;
  ip->mode = mode & 07777;
  ip->dirty = 1;
  icache_writeback(fs, ip);
  icache_release(ip);
  return 0;
}

/* ext2_hardlink -- create a hard link: add dirent for existing inode */
int ext2_hardlink(void *fs_data, const char *newpath, uint32_t src_ino) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  if (!fs || !ext2_mounted) return -30; /* EROFS */

  while (*newpath == '/') newpath++;

  char parent_path[256];
  const char *basename = path_parent(newpath, parent_path, sizeof(parent_path));
  if (basename[0] == '\0') return -1;

  struct ext2_inode_mem *parent = ext2_namei(fs, parent_path);
  if (!parent) return -2; /* ENOENT */
  if (parent->type != EXT2_TYPE_DIR) {
    icache_release(parent);
    return -20; /* ENOTDIR */
  }

  /* Check name doesn't already exist */
  uint32_t existing;
  if (ext2_dir_lookup(fs, parent, basename, &existing) == 0) {
    icache_release(parent);
    return -17; /* EEXIST */
  }

  /* Get source inode to determine file type for dirent */
  struct ext2_inode_mem *src = icache_get(fs, src_ino);
  if (!src) {
    icache_release(parent);
    return -2;
  }

  uint8_t ftype = EXT2_FT_FILE;
  if (src->type == EXT2_TYPE_DIR) ftype = EXT2_FT_DIR;
  else if (src->type == EXT2_TYPE_SYMLINK) ftype = EXT2_FT_SYMLINK;

  /* Add directory entry pointing to existing inode */
  int ret = ext2_dir_add(fs, parent, basename, src_ino, ftype);
  if (ret < 0) {
    icache_release(src);
    icache_release(parent);
    return ret;
  }

  /* Increment link count on source inode */
  src->link_count++;
  src->dirty = 1;
  icache_writeback(fs, src);

  icache_release(src);
  icache_release(parent);
  return 0;
}

void ext2_sync(void *fs_data) {
  struct ext2_fs *fs = (struct ext2_fs *)fs_data;
  if (!fs) {
    for (int i = 0; i < EXT2_MAX_FS; i++) {
      if (ext2_filesystems[i].mounted)
        ext2_sync(&ext2_filesystems[i]);
    }
    return;
  }
  if (!ext2_mounted) return;

  /* Write back all dirty icache entries */
  for (int i = 0; i < ICACHE_SIZE; i++) {
    if (icache[i].valid && icache[i].dirty)
      icache_writeback(fs, &icache[i]);
  }

  /* Write back all dirty bcache entries */
  bcache_sync(fs);

  /* Write superblock if dirty */
  sb_flush_if_dirty(fs);
}
