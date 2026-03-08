# ext2-lite: Read/Write Filesystem over virtio-blk

Design document for a minimal ext2-like on-disk filesystem for SBUnix.

Date: 2026-03-08
Status: Draft

---

## 1. On-Disk Layout

Block size is fixed at 4096 bytes (matches PAGE_SIZE). The virtio-blk device
reports capacity in 512-byte sectors; one filesystem block = 8 sectors.
`virtio_blk_rw()` already transfers 4096 bytes per call (descriptor 1 len = 4096),
so one `virtio_blk_rw()` call = one filesystem block.

### 1.1 Block Map

```
Block 0                Superblock
Block 1                Block bitmap   (1 bit per block; 4096*8 = 32768 blocks per bitmap block)
Block 2 .. B           Inode table    (fixed-size 128-byte inodes, packed)
Block B+1 .. end       Data blocks
```

For a small teaching disk (e.g. 64 MB = 16384 blocks), a single bitmap block
covers up to 32768 blocks, so one bitmap block suffices. The inode table size
is chosen at format time based on desired inode count.

### 1.2 Superblock (Block 0, bytes 0-4095)

Offset and size of each field within the 4096-byte superblock block:

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       4       magic               0x5BEF5101 ("SBext1" mangled)
4       4       block_size          Always 4096
8       4       total_blocks        Total blocks on the device
12      4       total_inodes        Total inode slots allocated
16      4       free_blocks         Number of free data blocks
20      4       free_inodes         Number of free inode slots
24      4       bitmap_start        Block number of first bitmap block (always 1)
28      4       bitmap_blocks       Number of blocks used for block bitmap (1 for <=32K blocks)
32      4       itable_start        Block number of first inode table block
36      4       itable_blocks       Number of blocks in the inode table
40      4       data_start          Block number of first data block
44      4       root_inode          Inode number of root directory (always 0)
48      4       version             Filesystem version (1)
52      4076    _reserved           Zero-padded to fill the 4096-byte block
```

All multi-byte fields are little-endian (native RISC-V byte order).

### 1.3 Block Bitmap (Block 1)

One bit per block. Bit 0 of byte 0 corresponds to block 0 (superblock).
Bit set = block is allocated. At format time, blocks 0 through `data_start - 1`
are marked allocated (metadata). Remaining bits are cleared (free).

With 4096 bytes per bitmap block, one block covers 32768 blocks (128 MB of
data). For larger disks, `bitmap_blocks` > 1 and they are contiguous starting
at block 1.

### 1.4 Inode Table (Blocks 2 .. B)

Inodes are 128 bytes each. 4096 / 128 = 32 inodes per block.

**Inode on-disk structure (128 bytes):**

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       2       type                0 = free, 1 = regular file, 2 = directory
2       2       link_count          Hard link count
4       4       size                File size in bytes (max ~50 MB with indirect)
8       4       direct[0..11]       12 direct block pointers (block numbers, 0 = unset)
56      4       indirect            Single indirect block pointer (0 = none)
60      4       _pad0               Reserved / alignment
64      8       ctime               Creation time (ticks or 0; optional for v1)
72      8       mtime               Modification time (optional for v1)
80      4       uid                 Owner (0 for v1)
84      4       gid                 Group (0 for v1)
88      4       mode                Permission bits (e.g. 0755)
92      36      _reserved           Zero-padded to 128 bytes
```

**Capacity per inode:**
- 12 direct blocks: 12 * 4096 = 49152 bytes (48 KB)
- 1 indirect block: 4096 / 4 = 1024 block pointers = 1024 * 4096 = 4 MB
- Maximum file size: 48 KB + 4 MB = ~4.1 MB (sufficient for a teaching OS)

**Inode numbering:** Inode 0 is always the root directory. Inode numbers are
indices into the inode table. Given inode number `ino`:
- Block = `itable_start + (ino * 128) / 4096`
- Offset within block = `(ino * 128) % 4096`

### 1.5 Directory Entry Format

Directories are stored as regular data blocks. Each directory file contains a
linear list of variable-length entries:

```
Offset  Size    Field
------  ----    -----
0       4       inode_number        Inode this entry refers to (0 = deleted/empty)
4       2       rec_len             Total record length (for skipping; aligned to 4 bytes)
6       1       name_len            Length of the name (no null terminator counted)
7       1       file_type           1 = file, 2 = directory (mirrors inode type)
8       N       name[]              File name (NOT null-terminated on disk)
```

`rec_len` is always >= 12 (minimum: 4 + 2 + 1 + 1 + 4 for a 4-char name) and
is 4-byte aligned. The last entry in a block has `rec_len` extending to the end
of the block (absorbing any free space).

When an entry is deleted, its `inode_number` is set to 0 and its `rec_len` is
merged into the previous entry's `rec_len` (or kept as-is if it is the first
entry, with `inode_number = 0` marking it as a tombstone). This is the standard
ext2 deletion strategy.

A freshly created directory contains two entries: `.` (self) and `..` (parent).

### 1.6 Example Layout for a 64 MB Disk

```
Total sectors: 131072 (64 MB / 512)
Total blocks:  16384  (64 MB / 4096)

Block 0:        Superblock
Block 1:        Block bitmap (covers blocks 0-32767; only 0-16383 valid)
Blocks 2-9:     Inode table (8 blocks * 32 inodes/block = 256 inodes)
Blocks 10-16383: Data blocks (16374 blocks = ~64 MB usable)

Superblock fields:
  total_blocks  = 16384
  total_inodes  = 256
  bitmap_start  = 1
  bitmap_blocks = 1
  itable_start  = 2
  itable_blocks = 8
  data_start    = 10
  root_inode    = 0
```

---

## 2. In-Memory Structures

### 2.1 Cached Superblock

```c
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
```

Read from disk at mount time. Written back on `sync` or `unmount`, or whenever
`dirty` is set and a convenient flush point is reached (e.g., after block
alloc/free).

### 2.2 Block Cache

```c
#define BCACHE_SIZE 16   /* number of cached blocks */

struct bcache_entry {
    uint32_t blockno;     /* disk block number, or 0 = empty slot */
    int      valid;       /* data has been read from disk */
    int      dirty;       /* data has been modified, needs writeback */
    int      refcount;    /* number of active users */
    uint8_t  data[4096];  /* block contents */
};

static struct bcache_entry bcache[BCACHE_SIZE];
```

**Eviction policy:** Clock (second-chance) algorithm. Walk the array; skip
entries with `refcount > 0`. Among unreferenced entries, prefer clean entries
over dirty ones (to avoid a write on eviction). If a dirty entry must be
evicted, write it back first.

**Interface:**
- `bcache_get(blockno)` -- returns pointer to a `bcache_entry` with `refcount++`.
  If not cached, evicts a slot, reads from disk, marks valid.
- `bcache_release(entry)` -- decrements `refcount`.
- `bcache_mark_dirty(entry)` -- sets `dirty = 1`.
- `bcache_sync()` -- writes all dirty entries back to disk.

The 16-entry cache occupies 16 * 4096 = 64 KB of static memory. This is
allocated as a static array to avoid fragmentation. If memory is tight, this
can be reduced to 8 entries (32 KB). Each entry embeds the 4096-byte buffer
directly rather than using `kalloc()` pages, keeping the design simple and
avoiding page allocator pressure.

### 2.3 Inode Cache

```c
#define ICACHE_SIZE 32

struct ext2_inode_mem {
    uint32_t ino;         /* inode number */
    int      valid;       /* has been read from disk */
    int      dirty;       /* needs writeback */
    int      refcount;    /* open file references */

    /* Cached on-disk fields: */
    uint16_t type;
    uint16_t link_count;
    uint32_t size;
    uint32_t direct[12];
    uint32_t indirect;
    uint32_t mode;
};

static struct ext2_inode_mem icache[ICACHE_SIZE];
```

Looked up by inode number. `refcount` tracks how many `open_file` descriptors
reference this inode. An inode with `refcount == 0` and `link_count == 0`
should have its blocks freed (deferred delete).

### 2.4 Private Data for Open Files

```c
struct ext2_open_private {
    struct ext2_inode_mem *inode;  /* pointer into icache */
    int      is_dir;              /* opened as directory */
};
```

Stored in `open_file->private`. The offset is already tracked by
`open_file->offset`.

---

## 3. Key Functions

### 3.1 Block I/O

```
int ext2_read_block(uint32_t blockno, void *buf)
```
- Convert `blockno` to sector: `sector = blockno * 8`.
- Call `virtio_blk_rw(sector, buf, 0)`.
- Returns 0 on success, -1 on error.

```
int ext2_write_block(uint32_t blockno, const void *buf)
```
- Convert `blockno` to sector: `sector = blockno * 8`.
- Call `virtio_blk_rw(sector, (void *)buf, 1)`.
- Returns 0 on success, -1 on error.

These are low-level functions called by the block cache, never directly by
VFS ops. All higher-level code goes through `bcache_get` / `bcache_release`.

### 3.2 Block Allocation

```
int ext2_alloc_block(void)
```
1. Read the bitmap block(s) through the block cache.
2. Scan for the first zero bit starting from `data_start`.
3. Set the bit, mark the bitmap cache entry dirty.
4. Decrement `superblock.free_blocks`, mark superblock dirty.
5. Return the block number. Return -1 if disk is full.

The allocated block's contents should be zeroed by the caller if needed (e.g.,
for indirect blocks or new directory blocks).

```
void ext2_free_block(uint32_t blockno)
```
1. Clear the bit in the bitmap. Mark bitmap cache entry dirty.
2. Increment `superblock.free_blocks`, mark superblock dirty.

### 3.3 Inode Allocation

```
int ext2_alloc_inode(uint16_t type)
```
1. Scan the inode table (via block cache) for an inode with `type == 0` (free).
2. Initialize the inode: set type, link_count = 1, size = 0, clear all block
   pointers.
3. Mark the inode table cache entry dirty.
4. Decrement `superblock.free_inodes`, mark superblock dirty.
5. Load the inode into the icache, set `refcount = 1`.
6. Return the inode number. Return -1 if no free inodes.

```
void ext2_free_inode(uint32_t ino)
```
1. Free all data blocks (direct + indirect) owned by the inode via
   `ext2_free_block()`.
2. If `indirect != 0`, read the indirect block, free all referenced data
   blocks, then free the indirect block itself.
3. Zero the inode's on-disk slot, set `type = 0`.
4. Mark the inode table cache entry dirty.
5. Increment `superblock.free_inodes`, mark superblock dirty.
6. Evict from icache.

### 3.4 Inode Block Mapping

```
uint32_t ext2_bmap(struct ext2_inode_mem *ip, uint32_t file_blockno)
```
Given a file-relative block number (0, 1, 2, ...), return the disk block
number:
- If `file_blockno < 12`: return `ip->direct[file_blockno]`.
- If `file_blockno < 12 + 1024`: read the indirect block, index into it at
  `(file_blockno - 12)`, return that block number.
- If block pointer is 0: block not yet allocated (sparse file / hole).

```
int ext2_bmap_alloc(struct ext2_inode_mem *ip, uint32_t file_blockno)
```
Same as `ext2_bmap`, but allocates blocks on demand:
- If the direct/indirect pointer is 0, call `ext2_alloc_block()`, store the
  new block number in the pointer, zero the new block.
- If the indirect block itself doesn't exist, allocate it first, zero it.
- Mark the inode dirty.
- Return the allocated disk block number, or -1 on failure.

### 3.5 Directory Operations

```
int ext2_dir_lookup(struct ext2_inode_mem *dir, const char *name,
                    uint32_t *out_ino)
```
Walk the directory's data blocks linearly. For each entry, compare `name_len`
and `name[]` against the search name. Return 0 and set `*out_ino` on match,
-ENOENT if not found.

```
int ext2_dir_add(struct ext2_inode_mem *dir, const char *name,
                 uint32_t ino, uint8_t file_type)
```
Find free space in an existing directory block (a deleted entry with enough
`rec_len`, or slack at the end of the last entry). If no space exists, allocate
a new block for the directory and append the entry there. Update `dir->size`
if a new block is added.

```
int ext2_dir_remove(struct ext2_inode_mem *dir, const char *name)
```
Find the entry, set its `inode_number = 0`, merge `rec_len` with the previous
entry if possible.

### 3.6 Path Resolution

```
int ext2_namei(const char *path, struct ext2_inode_mem **out)
```
Start from the root inode (inode 0). Split path on `/`. For each component,
call `ext2_dir_lookup` on the current directory inode. Load the resulting
inode from icache (or disk). Return the final inode with `refcount++`.

This function is called by `ext2_open`, `ext2_stat`, `ext2_mkdir`,
`ext2_unlink`. The path passed in is already relative to the `/disk` mount
point (the VFS layer strips the mount prefix).

---

## 4. VFS Integration

### 4.1 Registration

In `vfs_init()` (or a new `ext2_init()` called from `boot()`):

```c
extern struct vfs_ops ext2_ops;
vfs_register_mount("/disk", &ext2_ops);
```

This mounts the ext2-lite filesystem at `/disk`. Paths like `/disk/hello.txt`
will have the mount prefix stripped, so ext2 sees `hello.txt`.

The header `kernel/include/vfs.h` needs a new extern:

```c
extern struct vfs_ops ext2_ops;
```

### 4.2 VFS Ops Implementation

**ext2_open(path, flags, private_out):**
1. Call `ext2_namei(path)` to resolve the inode. If O_CREAT is set and
   the file doesn't exist, create it (allocate inode, add directory entry).
2. If O_TRUNC is set and the file exists, truncate it (free all data blocks,
   set size = 0).
3. Allocate an `ext2_open_private`, point it at the icache entry
   (`refcount++`).
4. Store in `*private_out`.
5. Return 0 on success.

**ext2_read(private, buf, count, offset):**
1. Get the inode from `private`.
2. Clamp `count` to `inode->size - *offset`.
3. Loop: compute `file_blockno = *offset / 4096`, offset within block.
   Call `ext2_bmap()` to get disk block. Read via bcache. Copy to `buf`.
   Advance `*offset`.
4. Return bytes read.

**ext2_write(private, buf, count, offset):**
1. Get the inode from `private`.
2. Loop: compute `file_blockno = *offset / 4096`. Call `ext2_bmap_alloc()`
   to get (or allocate) the disk block. Read existing block via bcache
   (unless writing a full block). Copy from `buf` into the cached block.
   Mark dirty. Advance `*offset`.
3. Update `inode->size` if `*offset > inode->size`. Mark inode dirty.
4. Return bytes written.

**ext2_close(private):**
1. Decrement the icache entry's `refcount`.
2. If `refcount == 0` and `link_count == 0`, call `ext2_free_inode()`.
3. If the inode is dirty, write it back.
4. Free the `ext2_open_private` struct via `kmfree()`.

**ext2_stat(path, st_buf):**
1. Call `ext2_namei(path)` to get the inode.
2. Fill in the 128-byte stat buffer (same layout as tarfs_stat):
   - `st + 16`: mode (S_IFREG 0x8000 | perms, or S_IFDIR 0x4000 | perms)
   - `st + 20`: nlink
   - `st + 48`: size (int64_t)
   - `st + 56`: blksize (4096)
   - `st + 64`: blocks (size / 512, for stat compatibility)
3. Release the inode (`refcount--`).

**ext2_readdir(private, buf, count, offset):**
1. Walk the directory's data blocks starting from `*offset`.
2. For each on-disk directory entry with `inode_number != 0`, emit a
   Linux-style `struct linux_dirent64` into `buf` (same format as tarfs and
   devfs: 8-byte ino, 8-byte d_off, 2-byte d_reclen, 1-byte d_type,
   then name).
3. Update `*offset` to the byte position in the directory data.
4. Return total bytes written into `buf`.

**ext2_mkdir(path, mode):**
1. Parse path into parent directory path + new directory name.
2. Resolve parent via `ext2_namei()`. Verify it is a directory.
3. Check that the name doesn't already exist in the parent.
4. Allocate a new inode (type = directory).
5. Allocate a data block for the new directory. Write `.` and `..` entries.
6. Add an entry for the new directory in the parent directory.
7. Release parent inode.

**ext2_unlink(path):**
1. Parse path into parent + name.
2. Resolve parent via `ext2_namei()`.
3. Look up name in parent to get the target inode number.
4. Load target inode. Decrement `link_count`. If `link_count == 0` and
   `refcount == 0`, free the inode immediately. If `refcount > 0`, defer
   deletion to `ext2_close()`.
5. Remove the directory entry from the parent.
6. Release both inodes.
7. Refuse to unlink directories (use rmdir for that, or simply disallow for v1).

### 4.3 Ops Struct

```c
struct vfs_ops ext2_ops = {
    .open    = ext2_open,
    .read    = ext2_read,
    .write   = ext2_write,
    .close   = ext2_close,
    .stat    = ext2_stat,
    .readdir = ext2_readdir,
    .mkdir   = ext2_mkdir,
    .unlink  = ext2_unlink,
};
```

---

## 5. mkfs Strategy

### 5.1 Recommended: Kernel-Side Format on First Boot

At boot, after `virtio_blk_init()` succeeds:
1. Read block 0.
2. Check if `magic == 0x5BEF5101`.
3. If not, call `ext2_mkfs()` to format the disk.

**ext2_mkfs() procedure:**
1. Compute layout from `blkdev.capacity`:
   - `total_blocks = capacity / 8` (sectors to 4K blocks).
   - `total_inodes = min(total_blocks / 4, 4096)` (reasonable ratio).
   - `bitmap_blocks = (total_blocks + 32767) / 32768`.
   - `itable_blocks = (total_inodes * 128 + 4095) / 4096`.
   - `data_start = 1 + bitmap_blocks + itable_blocks`.
2. Zero and write the superblock with computed values.
3. Zero and write the bitmap block(s). Mark blocks `0 .. data_start - 1`
   as allocated.
4. Zero all inode table blocks.
5. Initialize inode 0 as a directory (type = 2, link_count = 1, size = one
   block). Allocate its first data block (= `data_start`).
6. Write `.` and `..` entries (both pointing to inode 0) into that data block.
7. Mark the root directory's data block as allocated in the bitmap.
8. Write everything to disk.

This approach requires no host tools and makes the OS self-contained.

### 5.2 Alternative: Host-Side Tool

A standalone C program (compiled with the host gcc, not the cross compiler)
that writes directly to the QEMU disk image file:

```
./mkfs_ext2lite disk.img 64M
```

This is cleaner for development (can inspect the image with hex editors) but
adds a build step. Not recommended as the primary approach because the
kernel-side format is simpler to integrate and test.

### 5.3 Hybrid Approach

Support both: the kernel auto-formats on first boot, AND provide a host tool
for creating pre-populated images during development. The host tool can be
added later as a convenience.

---

## 6. Handling Non-Reentrant virtio_blk_rw()

### 6.1 The Problem

`virtio_blk_rw()` uses a `static struct virtio_blk_req` and a `static uint8_t
status_byte`. It reuses fixed descriptor indices 0-2. This means:
- Only one I/O request can be in flight at a time.
- If two calls overlap (e.g., from concurrent kernel threads or from a nested
  call within a signal handler), the static state will be corrupted.

### 6.2 Solution: Serialization via Spinlock

Introduce a spinlock that protects all calls to `virtio_blk_rw()`:

```c
static int blk_lock = 0;  /* simple spinlock */

static void blk_lock_acquire(void) {
    while (__sync_lock_test_and_set(&blk_lock, 1))
        ;  /* spin */
}

static void blk_lock_release(void) {
    __sync_lock_release(&blk_lock);
}
```

All ext2 block I/O goes through `ext2_read_block()` / `ext2_write_block()`,
which acquire this lock before calling `virtio_blk_rw()` and release it after.

Since SBUnix is currently single-core (single hart) and uses cooperative
scheduling (or preemptive with timer but no SMP), the primary risk is
re-entrance from nested traps or signals. The spinlock with interrupts
disabled around the critical section prevents this:

```c
int ext2_read_block(uint32_t blockno, void *buf) {
    /* Disable interrupts to prevent preemption during I/O */
    uint64_t sstatus = csr_read(sstatus);
    csr_clear(sstatus, SSTATUS_SIE);

    blk_lock_acquire();
    int ret = virtio_blk_rw((uint64_t)blockno * 8, buf, 0);
    blk_lock_release();

    /* Restore interrupt state */
    csr_write(sstatus, sstatus);
    return ret;
}
```

### 6.3 Block Cache as Mitigation

The block cache dramatically reduces the number of `virtio_blk_rw()` calls.
Frequently accessed blocks (superblock, bitmap, hot inode table entries) stay
cached. The lock contention is minimal because actual disk I/O is infrequent
during normal operation.

### 6.4 Future: Make virtio_blk_rw Reentrant

To properly fix this in the future, the driver could:
- Use dynamically allocated request structs (from `kmalloc()`).
- Use different descriptor chain indices per request (the virtqueue has 16
  slots).
- Support interrupt-driven completion instead of polling.

This is out of scope for the current design.

---

## 7. Edge Cases and Pitfalls

### 7.1 Disk Full

`ext2_alloc_block()` returns -1. Write operations must check this and return
`-ENOSPC` (errno 28). Partially-written files are valid (the bytes already
written are committed). The caller sees a short write.

### 7.2 Inode Table Full

`ext2_alloc_inode()` returns -1. `open(O_CREAT)` and `mkdir()` must return
`-ENOSPC`.

### 7.3 Maximum File Size

With 12 direct + 1 indirect (1024 entries), max file = 12*4096 + 1024*4096
= 4,243,456 bytes (~4.05 MB). Writes beyond this must return -EFBIG or
-ENOSPC. The `ext2_bmap_alloc()` function checks
`file_blockno < 12 + 1024` and returns -1 if exceeded.

### 7.4 Directory Size Limits

Directories grow by allocating additional data blocks. With the same indirect
limit, a directory could hold entries across 1036 blocks. Each block holds
many entries (minimum ~12 bytes each), so this is not a practical limit.
However, linear scanning is O(n). Acceptable for a teaching OS.

### 7.5 Cache Coherency

Since there is only one mount of the filesystem and no concurrent access from
outside the kernel, cache coherency is straightforward: the block cache and
icache are the single source of truth while mounted. The only requirement is
that dirty entries are written back before shutdown.

### 7.6 Power Failure / Unclean Shutdown

No journaling. An unclean shutdown may leave:
- Allocated blocks not referenced by any inode (leaked blocks).
- Inode with `link_count = 0` but blocks still allocated.
- Partially written directory entries.

Mitigation for v1: none (teaching OS). A future `fsck` could scan all inodes
and rebuild the free bitmap. Alternatively, a simple write-ordering discipline
(write data blocks before metadata) reduces the blast radius.

### 7.7 Name Length

Maximum filename: 255 bytes (limited by `name_len` being a `uint8_t`).
Paths are already limited to 256 bytes elsewhere in the VFS layer.

### 7.8 Empty Path Components

`ext2_namei()` must handle consecutive slashes (`/disk//foo`) and trailing
slashes (`/disk/foo/`) gracefully. Skip empty components. A trailing slash
on a non-directory should return `-ENOTDIR`.

### 7.9 Deleting Open Files

When `unlink()` is called on a file that is still open (refcount > 0 in the
icache), the directory entry is removed but the inode and data blocks are not
freed. The file becomes invisible in the directory but remains accessible via
existing file descriptors. When the last descriptor is closed (`refcount`
drops to 0 and `link_count == 0`), `ext2_close()` calls `ext2_free_inode()`.

### 7.10 Root Directory Cannot Be Deleted

`ext2_unlink()` and any future `rmdir()` must refuse to remove inode 0.

### 7.11 Reading an Indirect Block That Is Not Cached

When `ext2_bmap()` needs to read the indirect block, it goes through the
block cache. If the indirect block is evicted and re-read, this is
transparent. However, the inode's `indirect` field must always be persisted
before the indirect block's contents are relied upon. Write ordering:
1. Write the indirect block data.
2. Write the inode (which contains the indirect block pointer).

### 7.12 O_APPEND

If the VFS or syscall layer supports O_APPEND, `ext2_write()` must set
`*offset = inode->size` before each write. The current VFS ops signature
receives `offset` as a pointer, so this is feasible.

---

## 8. Testing Approach

### 8.1 Unit-Level Tests (Kernel Shell Commands)

Add shell commands or test functions callable from the kernel main loop:

1. **test_mkfs**: Format the disk, verify superblock magic, read it back.
2. **test_alloc_free**: Allocate N blocks, verify bitmap bits are set. Free
   them, verify bits are cleared. Check free_blocks count.
3. **test_inode_alloc**: Allocate inodes, verify type is set. Free them,
   verify type is 0.
4. **test_create_write_read**: Create a file via ext2_open(O_CREAT), write
   known data, close, reopen, read back, compare.
5. **test_large_file**: Write > 48 KB to exercise the indirect block path.
   Read back and verify.
6. **test_mkdir_readdir**: Create directories, verify `.` and `..` entries.
   List contents via readdir.
7. **test_unlink**: Create a file, unlink it, verify it no longer appears in
   readdir. Verify blocks are freed (free_blocks increases).
8. **test_persistence**: Write a file, call `bcache_sync()`, then invalidate
   the entire block cache (mark all entries invalid). Re-read and verify the
   data survived the round-trip to disk.

### 8.2 Userspace Tests (from bin/)

Once the filesystem is mounted at `/disk`, test from userspace binaries:

```c
/* bin/disktest/disktest.c */
int fd = open("/disk/hello.txt", O_CREAT | O_WRONLY);
write(fd, "Hello ext2-lite!\n", 17);
close(fd);

fd = open("/disk/hello.txt", O_RDONLY);
char buf[64];
int n = read(fd, buf, sizeof(buf));
write(STDOUT_FILENO, buf, n);
close(fd);
```

Additional userspace tests:
- `ls /disk` (exercises opendir/readdir/closedir via busybox)
- `mkdir /disk/subdir`
- `echo data > /disk/subdir/file.txt`
- `cat /disk/subdir/file.txt`
- `rm /disk/subdir/file.txt`

### 8.3 Stress Tests

- Fill the disk completely, verify -ENOSPC is returned.
- Create the maximum number of inodes, verify -ENOSPC.
- Create and delete many files in a loop to test free-list correctness.
- Write a file larger than 48 KB (indirect block threshold) and verify.

### 8.4 Persistence Test

1. Boot, create files, shut down QEMU.
2. Boot again with the same disk image.
3. Verify the files are still present and contents are correct.

This tests that `bcache_sync()` (or automatic sync on shutdown) is working
and that the on-disk format is self-consistent.

### 8.5 Verification Strategy

Since there is no test harness, all tests print PASS/FAIL to the console.
The `virtio_blk_test()` function in the existing codebase is a good model:
perform an operation, read back, compare, print result.

---

## Appendix A: File Inventory

New files to create:

| File | Purpose |
|------|---------|
| `kernel/ext2.c` | All ext2 filesystem logic (block/inode alloc, dir ops, VFS ops) |
| `kernel/include/ext2.h` | On-disk structures, in-memory structures, function declarations |

Modified files:

| File | Change |
|------|--------|
| `kernel/include/vfs.h` | Add `extern struct vfs_ops ext2_ops;` |
| `kernel/vfs.c` | Add `vfs_register_mount("/disk", &ext2_ops);` in `vfs_init()` |
| `kernel/kernel.c` | Call `ext2_init()` after `virtio_blk_init()` |

---

## Appendix B: Implementation Order

Recommended order to implement and test incrementally:

1. **ext2_read_block / ext2_write_block** with the spinlock wrapper.
2. **Block cache** (bcache_get, bcache_release, bcache_sync).
3. **ext2_mkfs** -- format on first boot, verify by reading superblock back.
4. **ext2_alloc_block / ext2_free_block** -- bitmap manipulation.
5. **ext2_alloc_inode / ext2_free_inode** -- inode table manipulation.
6. **ext2_bmap / ext2_bmap_alloc** -- block mapping for files.
7. **ext2_dir_lookup / ext2_dir_add** -- directory entry operations.
8. **ext2_namei** -- path resolution.
9. **ext2_open / ext2_close** -- VFS open/close with icache management.
10. **ext2_read / ext2_write** -- file data transfer.
11. **ext2_stat** -- file metadata.
12. **ext2_readdir** -- directory listing.
13. **ext2_mkdir** -- directory creation.
14. **ext2_unlink** -- file deletion.
15. **VFS registration** and end-to-end testing.
