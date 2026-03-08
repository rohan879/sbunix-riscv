/*
 * mman.c — memory management syscall wrappers for SBUnix libc.
 *
 * mmap(), munmap(), and mprotect() wrap the kernel's SYS_mmap (222),
 * SYS_munmap (215), and SYS_mprotect (226) respectively.
 *
 * sbrk() and brk() are implemented in malloc.c because the heap
 * allocator depends on them directly.
 */

#include <sys/mman.h>
#include <syscall.h>

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset) {
    void *ret = (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    if ((long)ret < 0 && (long)ret > -4096)
        return MAP_FAILED;
    return ret;
}

int munmap(void *addr, size_t length) {
    return (int)syscall(SYS_munmap, addr, length);
}

int mprotect(void *addr, size_t length, int prot) {
    return (int)syscall(SYS_mprotect, addr, length, prot);
}

int madvise(void *addr, size_t length, int advice) {
    return (int)syscall(SYS_madvise, addr, length, advice);
}
