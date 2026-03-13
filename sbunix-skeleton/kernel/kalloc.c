#include <kalloc.h>
#include <printk.h>
#include <riscv.h>
#include <string.h>
#include <swap.h>

extern char _kernel_end[];

/* ---- Physical page metadata ---- */
#define NPAGE       ((PHYSTOP - 0x80000000UL) / PAGE_SIZE)  /* 32768 */
#define MAX_ORDER   10  /* orders 0..9: 4KB .. 2MB */

#define PA_TO_IDX(pa)     (((uint64_t)(pa) - 0x80000000UL) >> 12)
#define IDX_TO_PA(idx)    (0x80000000UL + ((uint64_t)(idx) << 12))
#define BUDDY_PA(pa, ord) ((uint64_t)(pa) ^ (1UL << (12 + (ord))))

static int      page_refcount[NPAGE];
static uint8_t  page_order[NPAGE];   /* order of block (head page only) */
static uint8_t  page_free[NPAGE];    /* 1 = head of a free buddy block */

/* ---- Buddy free lists ---- */
struct free_block {
    struct free_block *next;
};

static struct free_block *free_list[MAX_ORDER];
static int free_count[MAX_ORDER];

static int total_free_pages;
static uint64_t mem_start;  /* first usable PA (page-aligned, after kernel) */

/* ---- Page reference counting (API unchanged) ---- */

static int pa_to_index(uint64_t pa) {
    return (int)((pa - 0x80000000UL) / PAGE_SIZE);
}

void page_ref_inc(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < (int)NPAGE)
        page_refcount[idx]++;
}

void page_ref_dec(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < (int)NPAGE)
        page_refcount[idx]--;
}

int page_ref_get(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < (int)NPAGE)
        return page_refcount[idx];
    return 0;
}

void page_ref_set(uint64_t pa, int val) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < (int)NPAGE)
        page_refcount[idx] = val;
}

/* ---- Buddy allocator internals ---- */

static void buddy_list_insert(uint64_t pa, int order) {
    struct free_block *blk = (struct free_block *)pa;
    blk->next = free_list[order];
    free_list[order] = blk;
    free_count[order]++;
}

static int buddy_list_remove(uint64_t pa, int order) {
    struct free_block **pp = &free_list[order];
    while (*pp) {
        if ((uint64_t)*pp == pa) {
            *pp = (*pp)->next;
            free_count[order]--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

/*
 * Allocate a block of 2^order contiguous pages.
 * Returns physical address or NULL.
 */
static void *buddy_alloc(int order) {
    if (order >= MAX_ORDER)
        return NULL;

    /* Try this order's free list */
    if (free_list[order]) {
        struct free_block *blk = free_list[order];
        free_list[order] = blk->next;
        free_count[order]--;

        uint64_t pa = (uint64_t)blk;
        int idx = PA_TO_IDX(pa);
        page_free[idx] = 0;
        page_order[idx] = (uint8_t)order;
        total_free_pages -= (1 << order);
        return (void *)pa;
    }

    /* Split a higher-order block */
    void *higher = buddy_alloc(order + 1);
    if (!higher)
        return NULL;

    uint64_t pa = (uint64_t)higher;
    uint64_t buddy = pa + ((uint64_t)PAGE_SIZE << order);
    int buddy_idx = PA_TO_IDX(buddy);

    /* Upper buddy goes to free_list[order] */
    page_free[buddy_idx] = 1;
    page_order[buddy_idx] = (uint8_t)order;
    buddy_list_insert(buddy, order);
    total_free_pages += (1 << order);

    /* Lower half is the allocation */
    int idx = PA_TO_IDX(pa);
    page_free[idx] = 0;
    page_order[idx] = (uint8_t)order;
    return (void *)pa;
}

/*
 * Free a block of 2^order pages, coalescing with buddy if possible.
 */
static void buddy_free(uint64_t pa, int order) {
    while (order < MAX_ORDER - 1) {
        uint64_t buddy = BUDDY_PA(pa, order);
        int buddy_idx = PA_TO_IDX(buddy);

        if (buddy < mem_start ||
            buddy + ((uint64_t)PAGE_SIZE << order) > PHYSTOP ||
            buddy_idx >= (int)NPAGE ||
            !page_free[buddy_idx] ||
            page_order[buddy_idx] != (uint8_t)order ||
            page_refcount[buddy_idx] != 0)
            break;

        /* Coalesce */
        buddy_list_remove(buddy, order);
        page_free[buddy_idx] = 0;
        total_free_pages -= (1 << order);

        if (buddy < pa)
            pa = buddy;
        order++;
    }

    int idx = PA_TO_IDX(pa);
    page_free[idx] = 1;
    page_order[idx] = (uint8_t)order;
    buddy_list_insert(pa, order);
    total_free_pages += (1 << order);
}

/* ---- Order-0 magazine (fast path for single-page alloc/free) ---- */

#define MAGAZINE_SIZE 16
static void *magazine[MAGAZINE_SIZE];
static int magazine_count;

static void *magazine_alloc(void) {
    if (magazine_count > 0)
        return magazine[--magazine_count];

    /* Refill: try to get an order-4 block (16 pages) */
    void *blk = buddy_alloc(4);
    if (blk) {
        uint64_t base = (uint64_t)blk;
        for (int i = 1; i < 16; i++) {
            uint64_t pg = base + (uint64_t)i * PAGE_SIZE;
            int idx = PA_TO_IDX(pg);
            page_order[idx] = 0;
            page_free[idx] = 0;
            magazine[magazine_count++] = (void *)pg;
        }
        int idx = PA_TO_IDX(base);
        page_order[idx] = 0;
        page_free[idx] = 0;
        return (void *)base;
    }

    /* Fallback: single order-0 allocation */
    return buddy_alloc(0);
}

static void magazine_free(uint64_t pa) {
    if (magazine_count >= MAGAZINE_SIZE) {
        /* Drain half back to buddy */
        for (int i = 0; i < MAGAZINE_SIZE / 2; i++)
            buddy_free((uint64_t)magazine[--magazine_count], 0);
    }
    magazine[magazine_count++] = (void *)pa;
}

/* ---- Public API ---- */

void kfree(void *pa) {
    int idx = pa_to_index((uint64_t)pa);
    if (idx >= 0 && idx < (int)NPAGE) {
        if (page_refcount[idx] > 1) {
            page_refcount[idx]--;
            return;
        }
        page_refcount[idx] = 0;
    }
    magazine_free((uint64_t)pa);
}

void kinit(void) {
    memset(page_refcount, 0, sizeof(page_refcount));
    memset(page_order, 0, sizeof(page_order));
    memset(page_free, 0, sizeof(page_free));
    memset(free_list, 0, sizeof(free_list));
    memset(free_count, 0, sizeof(free_count));
    total_free_pages = 0;
    magazine_count = 0;

    mem_start = PGROUNDUP((uint64_t)_kernel_end);

    printk("kinit: initializing memory from 0x%lx to 0x%lx\n",
           (unsigned long)mem_start, (unsigned long)PHYSTOP);

    /* Add free memory as the largest possible buddy blocks.
     * At each position, find the highest order that is both
     * naturally aligned and fits within remaining memory. */
    uint64_t p = mem_start;
    while (p + PAGE_SIZE <= PHYSTOP) {
        int order = 0;
        for (int o = MAX_ORDER - 1; o >= 0; o--) {
            uint64_t block_size = (uint64_t)PAGE_SIZE << o;
            if ((p & (block_size - 1)) == 0 && p + block_size <= PHYSTOP) {
                order = o;
                break;
            }
        }

        uint64_t block_size = (uint64_t)PAGE_SIZE << order;
        int idx = PA_TO_IDX(p);
        page_free[idx] = 1;
        page_order[idx] = (uint8_t)order;
        page_refcount[idx] = 0;
        buddy_list_insert(p, order);
        total_free_pages += (1 << order);
        p += block_size;
    }

    printk("kinit: %d free pages (%d MB) in buddy allocator\n",
           total_free_pages, total_free_pages * 4 / 1024);
}

void *kalloc(void) {
    if (total_free_pages + magazine_count < SWAP_LOW_WATERMARK && swap_enabled())
        swap_evict_pages(SWAP_EVICT_BATCH);

    void *pa = magazine_alloc();
    if (pa)
        page_refcount[PA_TO_IDX((uint64_t)pa)] = 1;
    return pa;
}

int get_free_page_count(void) {
    return total_free_pages + magazine_count;
}

void *kalloc_super(void) {
    void *pa = buddy_alloc(9);
    if (pa)
        page_refcount[PA_TO_IDX((uint64_t)pa)] = 1;
    return pa;
}

void kfree_super(void *pa) {
    if (!IS_SUPERPAGE_ALIGNED((uint64_t)pa))
        return;

    int idx = pa_to_index((uint64_t)pa);
    if (idx >= 0 && idx < (int)NPAGE) {
        if (page_refcount[idx] > 1) {
            page_refcount[idx]--;
            return;
        }
        page_refcount[idx] = 0;
    }
    buddy_free((uint64_t)pa, 9);
}
