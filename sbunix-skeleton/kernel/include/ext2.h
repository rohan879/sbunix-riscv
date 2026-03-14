#ifndef _EXT2_H
#define _EXT2_H

#include <riscv.h>

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

/* Block cache size */
#define BCACHE_SIZE       16

/* Inode cache size */
#define ICACHE_SIZE       32

/* Maximum open ext2 files */
#define EXT2_MAX_OPEN     64

/* File flags (matching libc/include/fcntl.h) */
#define EXT2_O_RDONLY     0
#define EXT2_O_WRONLY     1
#define EXT2_O_RDWR       2
#define EXT2_O_CREAT      0100
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
  uint64_t ctime;
  uint64_t mtime;
};

/* ------------------------------------------------------------------ */
/* Per-open-file private data                                          */
/* ------------------------------------------------------------------ */
struct ext2_open_private {
  uint32_t ino;              /* inode number */
  int      is_dir;
};

/* ------------------------------------------------------------------ */
/* Public functions                                                    */
/* ------------------------------------------------------------------ */
int  ext2_init(void);
void ext2_sync(void);
int  ext2_mknod(const char *path, uint32_t mode, uint32_t rdev);
int  ext2_truncate_to(uint32_t ino, uint32_t new_size);

#endif
