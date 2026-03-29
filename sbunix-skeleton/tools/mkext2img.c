#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXT2_MAGIC        0x5BEF5101U
#define EXT2_BLOCK_SIZE   4096U
#define EXT2_SECTORS_PER_BLOCK 8U
#define EXT2_INODE_SIZE   128U
#define EXT2_INODES_PER_BLOCK 32U
#define EXT2_NDIRECT      12U
#define EXT2_NINDIRECT    1024U
#define EXT2_ROOT_INO     0U

#define EXT2_TYPE_FREE    0U
#define EXT2_TYPE_FILE    1U
#define EXT2_TYPE_DIR     2U
#define EXT2_TYPE_SYMLINK 5U

#define EXT2_FT_FILE      1U
#define EXT2_FT_DIR       2U
#define EXT2_FT_SYMLINK   7U

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
  int dirty;
};

struct ext2_dinode {
  uint16_t type;
  uint16_t link_count;
  uint32_t size;
  uint32_t direct[12];
  uint32_t indirect;
  uint32_t _pad0;
  uint64_t ctime;
  uint64_t mtime;
  uint32_t uid;
  uint32_t gid;
  uint32_t mode;
  uint32_t rdev;
  uint8_t _reserved[32];
} __attribute__((packed));

struct ext2_dirent {
  uint32_t inode_number;
  uint16_t rec_len;
  uint8_t name_len;
  uint8_t file_type;
  char name[];
} __attribute__((packed));

static uint8_t *image;
static uint64_t image_bytes;
static struct ext2_superblock sb;

static void die_errno(const char *msg) {
  perror(msg);
  exit(1);
}

static void die_msg(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  exit(1);
}

static uint8_t *block_ptr(uint32_t blockno) {
  return image + (uint64_t)blockno * EXT2_BLOCK_SIZE;
}

static struct ext2_dinode *inode_ptr(uint32_t ino) {
  uint64_t off = (uint64_t)sb.itable_start * EXT2_BLOCK_SIZE +
                 (uint64_t)ino * EXT2_INODE_SIZE;
  return (struct ext2_dinode *)(image + off);
}

static void sync_superblock(void) {
  uint8_t *blk0 = block_ptr(0);
  memset(blk0, 0, EXT2_BLOCK_SIZE);
  memcpy(blk0, &sb, 52);
}

static void bitmap_set(uint32_t blockno) {
  uint32_t bm_block = blockno / (EXT2_BLOCK_SIZE * 8U);
  uint32_t bit_idx = blockno % (EXT2_BLOCK_SIZE * 8U);
  uint8_t *bm = block_ptr(sb.bitmap_start + bm_block);
  bm[bit_idx / 8U] |= (uint8_t)(1U << (bit_idx % 8U));
}

static int bitmap_test(uint32_t blockno) {
  uint32_t bm_block = blockno / (EXT2_BLOCK_SIZE * 8U);
  uint32_t bit_idx = blockno % (EXT2_BLOCK_SIZE * 8U);
  uint8_t *bm = block_ptr(sb.bitmap_start + bm_block);
  return (bm[bit_idx / 8U] & (1U << (bit_idx % 8U))) != 0;
}

static uint32_t alloc_block(void) {
  for (uint32_t blockno = sb.data_start; blockno < sb.total_blocks; blockno++) {
    if (bitmap_test(blockno))
      continue;
    bitmap_set(blockno);
    sb.free_blocks--;
    memset(block_ptr(blockno), 0, EXT2_BLOCK_SIZE);
    return blockno;
  }
  die_msg("mkext2img: out of blocks");
  return 0;
}

static uint32_t alloc_inode(uint16_t type, uint32_t mode) {
  for (uint32_t ino = 1; ino < sb.total_inodes; ino++) {
    struct ext2_dinode *di = inode_ptr(ino);
    if (di->type != EXT2_TYPE_FREE)
      continue;
    memset(di, 0, sizeof(*di));
    di->type = type;
    di->link_count = 1;
    di->mode = mode;
    sb.free_inodes--;
    return ino;
  }
  die_msg("mkext2img: out of inodes");
  return 0;
}

static uint32_t dirent_size(uint32_t name_len) {
  return (8U + name_len + 3U) & ~3U;
}

static uint32_t inode_bmap(struct ext2_dinode *di, uint32_t file_block) {
  if (file_block < EXT2_NDIRECT)
    return di->direct[file_block];
  file_block -= EXT2_NDIRECT;
  if (file_block >= EXT2_NINDIRECT || di->indirect == 0)
    return 0;
  return ((uint32_t *)block_ptr(di->indirect))[file_block];
}

static uint32_t inode_bmap_alloc(struct ext2_dinode *di, uint32_t file_block) {
  if (file_block < EXT2_NDIRECT) {
    if (di->direct[file_block] == 0)
      di->direct[file_block] = alloc_block();
    return di->direct[file_block];
  }

  file_block -= EXT2_NDIRECT;
  if (file_block >= EXT2_NINDIRECT)
    die_msg("mkext2img: file too large for ext2-lite image");

  if (di->indirect == 0)
    di->indirect = alloc_block();

  uint32_t *ptrs = (uint32_t *)block_ptr(di->indirect);
  if (ptrs[file_block] == 0)
    ptrs[file_block] = alloc_block();
  return ptrs[file_block];
}

static void dir_add(uint32_t dir_ino, const char *name, uint32_t ino,
                    uint8_t file_type) {
  struct ext2_dinode *dir = inode_ptr(dir_ino);
  uint32_t name_len = (uint32_t)strlen(name);
  uint32_t needed = dirent_size(name_len);
  uint32_t nblocks = (dir->size + EXT2_BLOCK_SIZE - 1U) / EXT2_BLOCK_SIZE;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t disk_blk = inode_bmap(dir, b);
    uint8_t *blk = block_ptr(disk_blk);
    uint32_t off = 0;

    while (off < EXT2_BLOCK_SIZE) {
      struct ext2_dirent *de = (struct ext2_dirent *)(blk + off);
      if (de->rec_len == 0)
        break;

      if (de->inode_number == 0 && de->rec_len >= needed) {
        de->inode_number = ino;
        de->name_len = (uint8_t)name_len;
        de->file_type = file_type;
        memcpy(de->name, name, name_len);
        return;
      }

      {
        uint32_t actual = dirent_size(de->name_len);
        if (de->inode_number != 0 && de->rec_len >= actual + needed) {
          uint32_t new_rec_len = de->rec_len - actual;
          de->rec_len = (uint16_t)actual;

          de = (struct ext2_dirent *)(blk + off + actual);
          de->inode_number = ino;
          de->rec_len = (uint16_t)new_rec_len;
          de->name_len = (uint8_t)name_len;
          de->file_type = file_type;
          memcpy(de->name, name, name_len);
          return;
        }
      }

      off += de->rec_len;
    }
  }

  {
    uint32_t new_blk = inode_bmap_alloc(dir, nblocks);
    struct ext2_dirent *de = (struct ext2_dirent *)block_ptr(new_blk);
    memset(de, 0, EXT2_BLOCK_SIZE);
    de->inode_number = ino;
    de->rec_len = EXT2_BLOCK_SIZE;
    de->name_len = (uint8_t)name_len;
    de->file_type = file_type;
    memcpy(de->name, name, name_len);
    dir->size += EXT2_BLOCK_SIZE;
  }
}

static void write_file_data(struct ext2_dinode *di, int fd, uint64_t size) {
  uint64_t remaining = size;
  uint32_t file_block = 0;
  uint8_t buf[EXT2_BLOCK_SIZE];

  while (remaining > 0) {
    uint64_t chunk = remaining > EXT2_BLOCK_SIZE ? EXT2_BLOCK_SIZE : remaining;
    ssize_t nr = read(fd, buf, chunk);
    if (nr < 0)
      die_errno("read");
    if ((uint64_t)nr != chunk)
      die_msg("mkext2img: short read while importing file");
    memcpy(block_ptr(inode_bmap_alloc(di, file_block)), buf, chunk);
    remaining -= chunk;
    file_block++;
  }
  di->size = (uint32_t)size;
}

static uint32_t create_dir(uint32_t parent_ino, uint32_t mode) {
  uint32_t ino = alloc_inode(EXT2_TYPE_DIR, mode ? mode : 0755U);
  struct ext2_dinode *di = inode_ptr(ino);
  uint32_t blk = alloc_block();
  struct ext2_dirent *de;

  di->size = EXT2_BLOCK_SIZE;
  di->direct[0] = blk;

  de = (struct ext2_dirent *)block_ptr(blk);
  memset(de, 0, EXT2_BLOCK_SIZE);
  de->inode_number = ino;
  de->rec_len = 12;
  de->name_len = 1;
  de->file_type = EXT2_FT_DIR;
  de->name[0] = '.';

  de = (struct ext2_dirent *)(block_ptr(blk) + 12);
  de->inode_number = parent_ino;
  de->rec_len = (uint16_t)(EXT2_BLOCK_SIZE - 12);
  de->name_len = 2;
  de->file_type = EXT2_FT_DIR;
  de->name[0] = '.';
  de->name[1] = '.';

  return ino;
}

static void import_path(const char *host_path, uint32_t parent_ino,
                        const char *name);

static void import_dir_contents(const char *host_dir, uint32_t dir_ino) {
  DIR *dir = opendir(host_dir);
  struct dirent *entry;

  if (!dir)
    die_errno(host_dir);

  while ((entry = readdir(dir)) != NULL) {
    char child_path[4096];
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (snprintf(child_path, sizeof(child_path), "%s/%s", host_dir,
                 entry->d_name) >= (int)sizeof(child_path))
      die_msg("mkext2img: path too long");
    import_path(child_path, dir_ino, entry->d_name);
  }

  closedir(dir);
}

static void import_path(const char *host_path, uint32_t parent_ino,
                        const char *name) {
  struct stat st;
  uint32_t ino;
  uint32_t mode;

  if (lstat(host_path, &st) < 0)
    die_errno(host_path);

  mode = (uint32_t)(st.st_mode & 0777U);

  if (S_ISDIR(st.st_mode)) {
    ino = create_dir(parent_ino, mode ? mode : 0755U);
    dir_add(parent_ino, name, ino, EXT2_FT_DIR);
    import_dir_contents(host_path, ino);
    return;
  }

  if (S_ISREG(st.st_mode)) {
    int fd = open(host_path, O_RDONLY);
    if (fd < 0)
      die_errno(host_path);
    ino = alloc_inode(EXT2_TYPE_FILE, mode ? mode : 0644U);
    write_file_data(inode_ptr(ino), fd, (uint64_t)st.st_size);
    close(fd);
    dir_add(parent_ino, name, ino, EXT2_FT_FILE);
    return;
  }

  if (S_ISLNK(st.st_mode)) {
    char target[4096];
    ssize_t len = readlink(host_path, target, sizeof(target) - 1);
    if (len < 0)
      die_errno(host_path);
    target[len] = '\0';
    ino = alloc_inode(EXT2_TYPE_SYMLINK, 0777U);
    {
      struct ext2_dinode *di = inode_ptr(ino);
      di->size = (uint32_t)len;
      memcpy(block_ptr(inode_bmap_alloc(di, 0)), target, (size_t)len);
    }
    dir_add(parent_ino, name, ino, EXT2_FT_SYMLINK);
    return;
  }

  fprintf(stderr, "mkext2img: skipping unsupported file type: %s\n", host_path);
}

static void format_image(uint64_t size_bytes) {
  uint32_t total_blocks = (uint32_t)(size_bytes / EXT2_BLOCK_SIZE);
  uint32_t total_inodes;
  uint32_t bitmap_blocks;
  uint32_t itable_blocks;
  uint32_t data_start;
  struct ext2_dinode *root;
  struct ext2_dirent *de;

  if (total_blocks < 16)
    die_msg("mkext2img: disk too small");

  memset(&sb, 0, sizeof(sb));
  total_inodes = total_blocks / 4U;
  if (total_inodes < 16U) total_inodes = 16U;

  bitmap_blocks = (total_blocks + 32767U) / 32768U;
  itable_blocks =
      (total_inodes * EXT2_INODE_SIZE + EXT2_BLOCK_SIZE - 1U) / EXT2_BLOCK_SIZE;
  data_start = 1U + bitmap_blocks + itable_blocks;

  sb.magic = EXT2_MAGIC;
  sb.block_size = EXT2_BLOCK_SIZE;
  sb.total_blocks = total_blocks;
  sb.total_inodes = total_inodes;
  sb.free_blocks = total_blocks - data_start - 1U;
  sb.free_inodes = total_inodes - 1U;
  sb.bitmap_start = 1U;
  sb.bitmap_blocks = bitmap_blocks;
  sb.itable_start = 1U + bitmap_blocks;
  sb.itable_blocks = itable_blocks;
  sb.data_start = data_start;
  sb.root_inode = EXT2_ROOT_INO;
  sb.version = 1U;

  for (uint32_t i = 0; i <= data_start; i++)
    bitmap_set(i);

  root = inode_ptr(EXT2_ROOT_INO);
  memset(root, 0, sizeof(*root));
  root->type = EXT2_TYPE_DIR;
  root->link_count = 1;
  root->size = EXT2_BLOCK_SIZE;
  root->direct[0] = data_start;
  root->mode = 0755U;

  de = (struct ext2_dirent *)block_ptr(data_start);
  memset(de, 0, EXT2_BLOCK_SIZE);
  de->inode_number = EXT2_ROOT_INO;
  de->rec_len = 12;
  de->name_len = 1;
  de->file_type = EXT2_FT_DIR;
  de->name[0] = '.';

  de = (struct ext2_dirent *)(block_ptr(data_start) + 12);
  de->inode_number = EXT2_ROOT_INO;
  de->rec_len = (uint16_t)(EXT2_BLOCK_SIZE - 12);
  de->name_len = 2;
  de->file_type = EXT2_FT_DIR;
  de->name[0] = '.';
  de->name[1] = '.';

  sync_superblock();
}

int main(int argc, char **argv) {
  const char *image_path;
  const char *src_dir;
  long size_mb;
  FILE *f;

  if (argc != 4) {
    fprintf(stderr, "usage: mkext2img <image> <size_mb> <source_dir>\n");
    return 1;
  }

  image_path = argv[1];
  src_dir = argv[3];
  size_mb = atol(argv[2]);
  if (size_mb <= 0)
    die_msg("mkext2img: invalid size");

  image_bytes = (uint64_t)size_mb * 1024ULL * 1024ULL;
  image = calloc(1, (size_t)image_bytes);
  if (!image)
    die_errno("calloc");

  format_image(image_bytes);
  import_dir_contents(src_dir, EXT2_ROOT_INO);
  sync_superblock();

  f = fopen(image_path, "wb");
  if (!f)
    die_errno(image_path);
  if (fwrite(image, 1, (size_t)image_bytes, f) != image_bytes)
    die_errno("fwrite");
  fclose(f);
  free(image);
  return 0;
}
