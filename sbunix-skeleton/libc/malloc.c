/*
 * malloc.c — simple free-list heap allocator for SBUnix libc.
 *
 * Uses SYS_brk to grow the heap. The allocator maintains a singly-linked
 * free list of blocks. Each allocated block is prefixed with a header that
 * stores the block size and a pointer to the next free block.
 *
 * This is intentionally simple: suitable for a simple OS where programs
 * are short-lived and heap fragmentation is not a concern.
 */

#include <stdlib.h>
#include <sys/mman.h>
#include <syscall.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * sbrk / brk — raw heap expansion via SYS_brk
 * ---------------------------------------------------------------------- */

static void *heap_end = (void *)0;  /* current program break */

/*
 * brk() — set the program break to addr.
 * Returns 0 on success, -1 on failure.
 */
int brk(void *addr) {
    void *result = (void *)syscall(SYS_brk, addr);
    if (result == addr) {
        heap_end = addr;
        return 0;
    }
    errno = ENOMEM;
    return -1;
}

/*
 * sbrk() — move the program break by `increment` bytes.
 * Returns the previous break on success, (void *)-1 on failure.
 */
void *sbrk(long increment) {
    /* First call: discover the current break from the kernel. */
    if (heap_end == (void *)0)
        heap_end = (void *)syscall(SYS_brk, 0);

    void *old_break = heap_end;
    if (increment == 0)
        return old_break;
    uintptr_t old_addr = (uintptr_t)heap_end;
    uintptr_t new_addr;
    /* BF: Use unsigned address arithmetic so LONG_MIN is never negated. */
    if (increment > 0) {
        uintptr_t delta = (uintptr_t)increment;
        if (old_addr > UINTPTR_MAX - delta) {
            errno = ENOMEM;
            return (void *)-1;
        }
        new_addr = old_addr + delta;
    } else {
        uintptr_t delta = (uintptr_t)0 - (uintptr_t)increment;
        if (old_addr < delta) {
            errno = ENOMEM;
            return (void *)-1;
        }
        new_addr = old_addr - delta;
    }
    void *new_break = (void *)new_addr;

    void *result = (void *)syscall(SYS_brk, new_break);
    if (result != new_break) {
        errno = ENOMEM;
        return (void *)-1;
    }

    heap_end = new_break;
    return old_break;
}

/* -----------------------------------------------------------------------
 * Free-list allocator
 * ---------------------------------------------------------------------- */

/* Block header: stored immediately before the user-visible data. */
typedef struct block_hdr {
    size_t          size;   /* usable bytes (excluding this header) */
    struct block_hdr *next; /* next free block, or NULL if not on free list */
    int             free;   /* 1 if on the free list, 0 if allocated */
} block_hdr_t;

#define HDR_SIZE  sizeof(block_hdr_t)
#define ALIGN     16              /* keep allocations 16-byte aligned */
#define ALIGN_UP(n) (((n) + ALIGN - 1) & ~(size_t)(ALIGN - 1))

/* Head of the free list */
static block_hdr_t *free_list = (void *)0;

/*
 * find_free() — walk the free list for a block of at least `size` bytes.
 * Returns the found block (removed from the free list) or NULL.
 */
static block_hdr_t *find_free(size_t size) {
    block_hdr_t *prev = (void *)0;
    block_hdr_t *cur  = free_list;

    while (cur) {
        if (cur->size >= size) {
            /* Remove from free list */
            if (prev)
                prev->next = cur->next;
            else
                free_list = cur->next;
            cur->next = (void *)0;
            cur->free = 0;
            return cur;
        }
        prev = cur;
        cur  = cur->next;
    }
    return (void *)0;
}

/*
 * request_space() — grow the heap to accommodate `size` bytes of user data.
 * Returns a pointer to the new block header, or NULL on failure.
 */
static block_hdr_t *request_space(size_t size) {
    block_hdr_t *hdr = (block_hdr_t *)sbrk(HDR_SIZE + size);
    if (hdr == (void *)-1)
        return (void *)0;
    hdr->size = size;
    hdr->next = (void *)0;
    hdr->free = 0;
    return hdr;
}

void *malloc(size_t size) {
    if (size == 0)
        return (void *)0;

    size = ALIGN_UP(size);

    block_hdr_t *hdr = find_free(size);
    if (!hdr) {
        hdr = request_space(size);
        if (!hdr)
            return (void *)0;
    }
    return (char *)hdr + HDR_SIZE;
}

/* BF: Sentinel magic used by posix_memalign for over-aligned blocks.
 * Must match the value in stdlib_ext.c. */
#define ALIGNED_SENTINEL 0xA110CA7ED5E1F000ULL

void free(void *ptr) {
    if (!ptr)
        return;

    /* BF: Check for over-aligned block from posix_memalign.
     * If the sentinel magic is present 2 words before ptr, the real
     * allocation pointer is stored 1 word before ptr.  We must free
     * the original pointer instead of the aligned one, otherwise the
     * heap free list would be corrupted. */
    uint64_t *sentinel = (uint64_t *)ptr;
    if (sentinel[-2] == ALIGNED_SENTINEL) {
        ptr = ((void **)ptr)[-1];
    }

    block_hdr_t *hdr = (block_hdr_t *)((char *)ptr - HDR_SIZE);
    if (hdr->free) return; /* double-free protection */
    hdr->free = 1;
    /* Prepend to free list */
    hdr->next = free_list;
    free_list = hdr;
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)-1 / nmemb) return (void *)0; /* overflow */
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return (void *)0;
    }

    block_hdr_t *hdr = (block_hdr_t *)((char *)ptr - HDR_SIZE);
    if (hdr->size >= size)
        return ptr;  /* existing block is large enough */

    void *new_ptr = malloc(size);
    if (!new_ptr)
        return (void *)0;

    memcpy(new_ptr, ptr, hdr->size);
    free(ptr);
    return new_ptr;
}
