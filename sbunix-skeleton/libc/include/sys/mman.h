#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>
/* ENH: POSIX requires O_* flags to be visible from <sys/mman.h> for
 * shm_open().  Include fcntl.h to pull them in. */
#include <fcntl.h>

/* mmap() protection flags */
#define PROT_NONE   0x00  /* page cannot be accessed */
#define PROT_READ   0x01  /* page can be read */
#define PROT_WRITE  0x02  /* page can be written */
#define PROT_EXEC   0x04  /* page can be executed */

/* mmap() mapping flags */
#define MAP_SHARED    0x01   /* share changes */
#define MAP_PRIVATE   0x02   /* changes are private (copy-on-write) */
#define MAP_FIXED     0x10   /* map at exactly the given address */
#define MAP_ANONYMOUS 0x20   /* not backed by a file */
#define MAP_ANON      MAP_ANONYMOUS  /* alias */

/* ENH: mremap() flags for memory remapping support */
#define MREMAP_MAYMOVE 1

/* mmap() failure sentinel */
#define MAP_FAILED    ((void *)-1)

/* msync() flags */
#define MS_ASYNC    1   /* schedule async write */
#define MS_SYNC     4   /* perform synchronous write */
#define MS_INVALIDATE 2 /* invalidate cached pages */

/* mprotect(), madvise() */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4

/* ENH: POSIX-standard posix_madvise constants for os-test compliance */
#define POSIX_MADV_NORMAL     0
#define POSIX_MADV_RANDOM     1
#define POSIX_MADV_SEQUENTIAL 2
#define POSIX_MADV_WILLNEED   3
#define POSIX_MADV_DONTNEED   4

/* ENH: mlock/mlockall flags required by POSIX */
#define MCL_CURRENT 1
#define MCL_FUTURE  2

/* ENH: POSIX typed memory constants (optional feature, stubs for compliance) */
#define POSIX_TYPED_MEM_ALLOCATE        0x1
#define POSIX_TYPED_MEM_ALLOCATE_CONTIG 0x2
#define POSIX_TYPED_MEM_MAP_ALLOCATABLE 0x4

/* ENH: struct posix_typed_mem_info for posix_typed_mem_get_info() */
struct posix_typed_mem_info {
  size_t posix_tmi_length;
};

/*
 * Linux RISC-V syscall numbers for memory management.
 * These are used by the wrappers below; all route through brk/mmap.
 */
#define SYS_brk  214
#define SYS_mmap 222

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t length, int prot);
/* ENH: mremap declaration for memory remapping support */
void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...);
int   madvise(void *addr, size_t length, int advice);
void *sbrk(long increment);
int   brk(void *addr);
/* ENH: POSIX msync, mlock, and shm functions for os-test compliance */
int   msync(void *addr, size_t length, int flags);
int   mlock(const void *addr, size_t len);
int   munlock(const void *addr, size_t len);
int   mlockall(int flags);
int   munlockall(void);
int   posix_madvise(void *addr, size_t len, int advice);
int   shm_open(const char *name, int oflag, mode_t mode);
int   shm_unlink(const char *name);
int   posix_typed_mem_open(const char *name, int oflag, int tflag);
int   posix_typed_mem_get_info(int fildes, struct posix_typed_mem_info *info);
int   posix_mem_offset(const void *restrict addr, size_t len,
                       off_t *restrict off, size_t *restrict contig_len,
                       int *restrict fildes);

#endif /* _SYS_MMAN_H */
