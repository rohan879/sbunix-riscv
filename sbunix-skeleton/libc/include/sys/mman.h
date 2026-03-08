#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>

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
int   madvise(void *addr, size_t length, int advice);
void *sbrk(long increment);
int   brk(void *addr);

#endif /* _SYS_MMAN_H */
