#ifndef _EXT2_H
#define _EXT2_H

#include <riscv.h>

struct vfs_ops;

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define EXT2_MAGIC        0x5BEF5101
#define EXT2_BLOCK_SIZE   4096
#define EXT2_SECTORS_PER_BLOCK  8   /* 4096 / 512 */
#define EXT2_INODES_PER_BLOCK   32  /* 4096 / 128 */
#define EXT2_INODE_SIZE   128
#define EXT2_NDIRECT      12
#define EXT2_NINDIRECT    1024      /* 4096 / 4 */
#define EXT2_ROOT_INO     0
#define EXT2_VERSION      1

/* Inode types */
#define EXT2_TYPE_FREE    0
#define EXT2_TYPE_FILE    1
#define EXT2_TYPE_DIR     2
#define EXT2_TYPE_CHRDEV  3
#define EXT2_TYPE_BLKDEV  4
#define EXT2_TYPE_SYMLINK 5

/* Directory entry file types (mirrors inode type) */
#define EXT2_FT_FILE      1
#define EXT2_FT_DIR       2
#define EXT2_FT_CHRDEV    3
#define EXT2_FT_BLKDEV    4
#define EXT2_FT_SYMLINK   7

/* Max filename length */
#define EXT2_NAME_MAX     255

/* Block cache size — increased from 16 to 64 to reduce evictions on 128MB
 * disk; memory impact is ~256KB (64 * 4KB blocks), well within budget. */
#define BCACHE_SIZE       64

/* Inode cache size — increased from 32 to 128 to reduce repeated inode
 * reads during directory traversals and path resolution. */
#define ICACHE_SIZE       128

/* Maximum open ext2 files */
#define EXT2_MAX_OPEN     64

/* File flags (matching libc/include/fcntl.h) */
#define EXT2_O_RDONLY     0
#define EXT2_O_WRONLY     1
#define EXT2_O_RDWR       2
#define EXT2_O_CREAT      0100
/* BF: ext2 open must see O_EXCL for create-existing rejection. */
#define EXT2_O_EXCL       0200
#define EXT2_O_TRUNC      01000

/* ------------------------------------------------------------------ */
/* On-disk superblock (fits in first 52 bytes of block 0)              */
/* ------------------------------------------------------------------ */
struct ext2_superblock {
  uint32_t magic;
  uint32_t block_size;
  uint32_t total_blocks;
  uint32_t total_inodes;
  uint32_t free_blocks;
  uint32_t free_inodes;
  uint32_t bitmap_start;
  uint32_t bitmap_blocks;
  uint32_t itable_start;
  uint32_t itable_blocks;
  uint32_t data_start;
  uint32_t root_inode;
  uint32_t version;
  int      dirty;           /* in-memory only: needs writeback */
};

struct ext2_fs;

/* ------------------------------------------------------------------ */
/* On-disk inode (128 bytes packed)                                     */
/* ------------------------------------------------------------------ */
struct ext2_dinode {
  uint16_t type;            /* 0=free, 1=file, 2=dir */
  uint16_t link_count;
  uint32_t size;
  uint32_t direct[12];      /* direct block pointers */
  uint32_t indirect;         /* single indirect block pointer */
  uint32_t _pad0;
  uint64_t ctime;
  uint64_t mtime;
  uint32_t uid;
  uint32_t gid;
  uint32_t mode;
  uint32_t rdev;           /* device major/minor (for char/block devs) */
  uint8_t  _reserved[32];
};

/* ------------------------------------------------------------------ */
/* On-disk directory entry (variable length)                           */
/* ------------------------------------------------------------------ */
struct ext2_dirent {
  uint32_t inode_number;
  uint16_t rec_len;
  uint8_t  name_len;
  uint8_t  file_type;
  char     name[];           /* NOT null-terminated on disk */
};

/* ------------------------------------------------------------------ */
/* In-memory block cache entry                                         */
/* ------------------------------------------------------------------ */
struct bcache_entry {
  uint32_t blockno;
  int      valid;
  int      dirty;
  int      refcount;
  uint8_t  data[EXT2_BLOCK_SIZE];
};

/* ------------------------------------------------------------------ */
/* In-memory inode cache entry                                         */
/* ------------------------------------------------------------------ */
struct ext2_inode_mem {
  uint32_t ino;
  int      valid;
  int      dirty;
  int      refcount;

  /* Cached on-disk fields */
  uint16_t type;
  uint16_t link_count;
  uint32_t size;
  uint32_t direct[12];
  uint32_t indirect;
  uint32_t mode;
  uint32_t rdev;
  uint32_t uid;
  uint32_t gid;
  uint64_t ctime;
  uint64_t mtime;
};

/* ------------------------------------------------------------------ */
/* Per-open-file private data                                          */
/* ------------------------------------------------------------------ */
struct ext2_open_private {
  struct ext2_fs *fs;       /* owning filesystem instance */
  uint32_t ino;              /* inode number */
  int      is_dir;
};

/* Per-mount ext2 instance. */
struct ext2_fs {
  struct ext2_superblock sb;
  struct bcache_entry bcache[BCACHE_SIZE];
  struct ext2_inode_mem icache[ICACHE_SIZE];
  int mounted;
  int bcache_hand;
  int device_index;
  const char *mount_path;
};

/* ------------------------------------------------------------------ */
/* Public functions                                                    */
/* ------------------------------------------------------------------ */
int  ext2_init(void);
int  ext2_register_mounts(void);
int  ext2_is_ops(struct vfs_ops *ops);
void ext2_sync(void *fs_data);
int  ext2_mknod(void *fs_data, const char *path, uint32_t mode, uint32_t rdev);
int  ext2_ftruncate_ino(void *fs_data, uint32_t ino, uint64_t length);
int  ext2_fchmod_ino(void *fs_data, uint32_t ino, uint32_t mode);
int  ext2_lookup_inode(void *fs_data, const char *path, uint32_t *ino_out,
                       uint16_t *type_out);
int  ext2_hardlink(void *fs_data, const char *newpath, uint32_t src_ino);

#endif
