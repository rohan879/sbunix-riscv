#include <kalloc.h>
#include <kmalloc.h>
#include <printk.h>
#include <riscv.h>
#include <string.h>
#include <swap.h>

extern char _kernel_end[]; // Symbol from linker script

struct run {
    struct run *next;
};

struct {
    struct run *freelist;
} kmem;

static int free_page_count;

/* Reference counts for physical pages */
#define MAX_PAGES ((128 * 1024 * 1024) / PAGE_SIZE) /* 128MB / 4KB = 32768 */
static int page_refcount[MAX_PAGES];
static uint64_t page_last_free_ra[MAX_PAGES];
static int kfree_debug_warn_count;

/* Track page usage type for debugging */
#define PAGE_TYPE_FREE    0
#define PAGE_TYPE_DATA    1
#define PAGE_TYPE_PTPAGE  2  /* page table page */
#define PAGE_TYPE_KSTACK  3  /* kernel stack */
static uint8_t page_type[MAX_PAGES];

/* Process table high water mark tracking */
static int proc_highwater = 0;
static int proc_alloc_total = 0;
static int proc_exhaustion_count = 0;

static int pa_to_index(uint64_t pa) {
    return (int)((pa - 0x80000000UL) / PAGE_SIZE);
}

/* Mark a page as being used for a page table */
void page_mark_ptpage(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES) {
        unsigned long old = r_sstatus();
        w_sstatus(old & ~SSTATUS_SIE);
        if (page_type[idx] == PAGE_TYPE_PTPAGE) {
            /* Already marked as ptpage - this might indicate double-use */
            /* Only warn if we haven't seen too many warnings */
            if (kfree_debug_warn_count < 32) {
                printk("WARNING: page 0x%lx already marked as ptpage (ref=%d)\n",
                       pa, page_refcount[idx]);
            }
        }
        page_type[idx] = PAGE_TYPE_PTPAGE;
        w_sstatus(old);
    }
}

/* Check if a page is marked as a page table page */
int page_is_ptpage(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES)
        return page_type[idx] == PAGE_TYPE_PTPAGE;
    return 0;
}

/* Track process table usage */
void track_proc_alloc(int current_count) {
    proc_alloc_total++;
    if (current_count > proc_highwater)
        proc_highwater = current_count;
}

void track_proc_exhaustion(void) {
    proc_exhaustion_count++;
    if (proc_exhaustion_count <= 5) {
        printk("WARNING: process table exhausted! (count=%d, highwater=%d, total_allocs=%d)\n",
               proc_exhaustion_count, proc_highwater, proc_alloc_total);
    }
}

int get_proc_highwater(void) { return proc_highwater; }
int get_proc_exhaustion_count(void) { return proc_exhaustion_count; }

void page_ref_inc(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES) {
        unsigned long old = r_sstatus();
        w_sstatus(old & ~SSTATUS_SIE);
        page_refcount[idx]++;
        w_sstatus(old);
    }
}

void page_ref_dec(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES) {
        unsigned long old = r_sstatus();
        w_sstatus(old & ~SSTATUS_SIE);
        if (page_refcount[idx] > 0)
            page_refcount[idx]--;
        else
            printk("WARNING: page_ref_dec on pa 0x%lx with refcount=%d\n", pa, page_refcount[idx]);
        w_sstatus(old);
    }
}

int page_ref_get(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES)
        return page_refcount[idx];
    return 0;
}

void page_ref_set(uint64_t pa, int val) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES) {
        unsigned long old = r_sstatus();
        w_sstatus(old & ~SSTATUS_SIE);
        page_refcount[idx] = val;
        w_sstatus(old);
    }
}

// Frees a page of physical memory.
// Only actually frees when refcount reaches 0.
void kfree(void *pa) {
    struct run *r;
    uint64_t caller_ra = (uint64_t)__builtin_return_address(0);

    if (!pa) return;
    if ((uint64_t)pa % PAGE_SIZE != 0) return; /* alignment check */
    if ((uint64_t)pa < 0x80000000UL || (uint64_t)pa >= PHYSTOP) return; /* range check */

    int idx = pa_to_index((uint64_t)pa);

    if (kmalloc_is_managed_page(pa)) {
        printk("WARNING: kfree on kmalloc-managed page 0x%lx! Ignoring.\n", (uint64_t)pa);
        return;
    }

    if (idx >= 0 && idx < MAX_PAGES) {
        if (page_refcount[idx] > 1) {
            page_refcount[idx]--;
            return; /* still referenced */
        }
        if (page_refcount[idx] == 0) {
            if (kfree_debug_warn_count < 32) {
                printk("WARNING: double kfree pa=0x%lx prev_ra=0x%lx curr_ra=0x%lx type=%d\n",
                       (uint64_t)pa, page_last_free_ra[idx], caller_ra, page_type[idx]);
                kfree_debug_warn_count++;
            }
            return;
        }
        page_refcount[idx] = 0;
        page_last_free_ra[idx] = caller_ra;
        page_type[idx] = PAGE_TYPE_FREE;
    }

    r = (struct run*)pa;
    r->next = kmem.freelist;
    kmem.freelist = r;
    free_page_count++;
    
}

// Initializes the page allocator.
// Divides the memory from the end of the kernel to PHYSTOP into pages
// and adds them to the free list.
void kinit(void) {
    char *p;
    kmem.freelist = NULL;

    // Start page-aligning from the end of the kernel data.
    p = (char*)ROUNDUP((unsigned long)_kernel_end, PAGE_SIZE);

    printk("kinit: initializing memory from 0x%lx to 0x%lx\n", (unsigned long)p, (unsigned long)PHYSTOP);

    for (; p + PAGE_SIZE <= (char*)PHYSTOP; p += PAGE_SIZE) {
        int idx = pa_to_index((uint64_t)p);
        if (idx >= 0 && idx < MAX_PAGES)
            page_refcount[idx] = 1;
        kfree(p);
    }

    printk("kinit: memory initialized.\n");
}

// Allocates one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns NULL if the memory cannot be allocated.
void *kalloc(void) {
    struct run *r;

    if (free_page_count < SWAP_LOW_WATERMARK && swap_enabled())
        swap_evict_pages(SWAP_EVICT_BATCH);

    r = kmem.freelist;
    if(r) {
        kmem.freelist = r->next;
        free_page_count--;
        page_refcount[pa_to_index((uint64_t)r)] = 1;
    }
    return r;
}

int get_free_page_count(void) {
    return free_page_count;
}

// Allocate 512 contiguous 4KB pages (2MB) at a 2MB-aligned physical address.
// Returns the base address or NULL if no contiguous block is available.
// Strategy: scan the free list, build a bitmap of free pages, then search
// for a 2MB-aligned run of 512 consecutive free pages.
void *kalloc_super(void) {
    /* Build a bitmap of which pages are on the free list.
     * MAX_PAGES = 32768, so bitmap = 4096 bytes = 1 page.
     * Static to avoid putting 4096 bytes on the 16KB kernel stack. */
    static uint8_t free_bitmap[MAX_PAGES / 8];

    unsigned long old = r_sstatus();
    w_sstatus(old & ~SSTATUS_SIE);

    memset(free_bitmap, 0, sizeof(free_bitmap));

    struct run *r = kmem.freelist;
    while (r) {
        int idx = pa_to_index((uint64_t)r);
        if (idx >= 0 && idx < MAX_PAGES)
            free_bitmap[idx / 8] |= (uint8_t)(1 << (idx % 8));
        r = r->next;
    }

    /* Search for a 2MB-aligned block of 512 contiguous free pages.
     * base must be a multiple of SUPERPAGE_NPAGES (512). */
    for (int base = 0; base + SUPERPAGE_NPAGES <= MAX_PAGES;
         base += SUPERPAGE_NPAGES) {
        int all_free = 1;
        for (int i = 0; i < SUPERPAGE_NPAGES; i++) {
            int idx = base + i;
            if (!(free_bitmap[idx / 8] & (1 << (idx % 8)))) {
                all_free = 0;
                break;
            }
        }
        if (!all_free)
            continue;

        /* Found a contiguous block. Remove all 512 pages from free list. */
        uint64_t base_pa = 0x80000000UL + (uint64_t)base * PAGE_SIZE;

        /* Clear all refcounts first */
        for (int i = 0; i < SUPERPAGE_NPAGES; i++)
            page_refcount[base + i] = 0;

        /* Remove the 512 pages from the free list */
        struct run **pp = &kmem.freelist;
        int removed = 0;
        while (*pp && removed < SUPERPAGE_NPAGES) {
            uint64_t addr = (uint64_t)*pp;
            if (addr >= base_pa && addr < base_pa + SUPERPAGE_SIZE) {
                *pp = (*pp)->next;
                removed++;
            } else {
                pp = &(*pp)->next;
            }
        }
        free_page_count -= removed;

        /* Set base page refcount to 1 */
        page_refcount[base] = 1;
        w_sstatus(old);
        return (void *)base_pa;
    }

    w_sstatus(old);
    return NULL; /* no contiguous 2MB block available */
}

// Free a 2MB contiguous block back to the free list.
// pa must be 2MB-aligned.  After superpage_split, individual sub-pages
// may have independent refcounts, so check each one before freeing.
void kfree_super(void *pa) {
    uint64_t base_pa = (uint64_t)pa;
    if (!IS_SUPERPAGE_ALIGNED(base_pa))
        return;
    if (base_pa < 0x80000000UL || base_pa + SUPERPAGE_SIZE > PHYSTOP)
        return;
    int base_idx = pa_to_index(base_pa);

    unsigned long old = r_sstatus();
    w_sstatus(old & ~SSTATUS_SIE);

    if (page_refcount[base_idx] > 1) {
        page_refcount[base_idx]--;
        w_sstatus(old);
        return;
    }
    if (page_refcount[base_idx] == 0) {
        printk("WARNING: double kfree_super on pa 0x%lx\n", base_pa);
        w_sstatus(old);
        return;
    }

    /* Return constituent 4KB pages to the free list, checking each
     * sub-page's refcount individually (superpage_split may have
     * given them independent tracking). */
    for (int i = 0; i < SUPERPAGE_NPAGES; i++) {
        uint64_t sub_pa = base_pa + (uint64_t)i * PAGE_SIZE;
        int idx = pa_to_index(sub_pa);
        if (idx >= 0 && idx < MAX_PAGES) {
            if (page_refcount[idx] > 1) {
                page_refcount[idx]--;  /* Still shared, just decrement */
                continue;
            }
            page_refcount[idx] = 0;
            page_type[idx] = PAGE_TYPE_FREE;
            struct run *r = (struct run *)sub_pa;
            r->next = kmem.freelist;
            kmem.freelist = r;
            free_page_count++;
        }
    }
    w_sstatus(old);
}
