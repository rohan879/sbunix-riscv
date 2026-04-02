#include <kalloc.h>
#include <kmalloc.h>
#include <printk.h>
#include <riscv.h>
#include <string.h>
#include <swap.h>
#include "include/mm.h"

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

static int pa_to_index(uint64_t pa) {
    return (int)((pa - 0x80000000UL) / PAGE_SIZE);
}

void page_ref_inc(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES)
        page_refcount[idx]++;
}

void page_ref_dec(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES)
        page_refcount[idx]--;
}

int page_ref_get(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES)
        return page_refcount[idx];
    return 0;
}

void page_ref_set(uint64_t pa, int val) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES)
        page_refcount[idx] = val;
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
                printk("WARNING: double kfree pa=0x%lx prev_ra=0x%lx curr_ra=0x%lx\n",
                       (uint64_t)pa, page_last_free_ra[idx], caller_ra);
                kfree_debug_warn_count++;
            }
            return;
        }
        page_refcount[idx] = 0;
        page_last_free_ra[idx] = caller_ra;
    }

    r = (struct run*)pa;
    r->next = kmem.freelist;
    kmem.freelist = r;
    free_page_count++;
    
    // Update mm_stats if page descriptors are available
    if (page_table) {
        mm_stats.free_pages = free_page_count;
    }
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

    // Initialize enhanced page descriptor system first
    mm_init_page_descriptors();

    for (; p + PAGE_SIZE <= (char*)PHYSTOP; p += PAGE_SIZE) {
        int idx = pa_to_index((uint64_t)p);
        if (idx >= 0 && idx < MAX_PAGES)
            page_refcount[idx] = 1;
        kfree(p);
    }

    // Initialize memory pressure watermarks after pages are set up
    mm_init_watermarks();

    printk("kinit: memory initialized with enhanced page descriptors and pressure detection.\n");
}

// Original kalloc for internal use and compatibility
static void *kalloc_internal(void) {
    struct run *r;

    // Enhanced memory pressure detection
    if (mm_watermarks.min_free > 0) {  // Check if watermarks initialized
        int pressure = mm_check_memory_pressure();
        if (pressure > 0) {
            mm_stats.pressure_events++;
            mm_trigger_reclaim(1);  // Try to reclaim pages
        }
    } else if (free_page_count < SWAP_LOW_WATERMARK && swap_enabled()) {
        // Fallback to original pressure handling if watermarks not initialized
        swap_evict_pages(SWAP_EVICT_BATCH);
    }

    r = kmem.freelist;
    if(r) {
        kmem.freelist = r->next;
        free_page_count--;
        page_refcount[pa_to_index((uint64_t)r)] = 1;
        
        // Update mm_stats if available
        if (page_table) {
            mm_stats.free_pages = free_page_count;
        }
    }
    return r;
}

// New enhanced kalloc with page descriptor integration
void *kalloc(void) {
    // Use enhanced kalloc if page descriptors are available
    if (page_table) {
        return enhanced_kalloc();
    }
    // Fallback to original kalloc during early boot
    return kalloc_internal();
}

// Enhanced kalloc implementation (moved from mm.c for integration)
void *enhanced_kalloc(void) {
    // Check for memory pressure before allocation
    int pressure = mm_check_memory_pressure();
    if (pressure >= 2) {  // High or critical pressure
        mm_stats.pressure_events++;
        if (mm_direct_reclaim(1) < 0) {
            // Direct reclaim failed - allocation might fail
            printk("mm: allocation may fail due to memory pressure\n");
        }
    } else if (pressure == 1) {  // Moderate pressure
        mm_stats.pressure_events++;
        mm_trigger_reclaim(1);  // Background reclaim
    }
    
    void *pa = kalloc_internal();  // Use internal allocator
    if (!pa) {
        // Allocation failed - try emergency reclaim
        if (mm_direct_reclaim(4) >= 0) {  // Try harder
            pa = kalloc_internal();  // Retry allocation
        }
        if (!pa) {
            printk("mm: enhanced_kalloc failed despite reclaim attempts\n");
            return NULL;
        }
    }
    
    // Initialize page descriptor for newly allocated page
    struct page *page = virt_to_page(pa);
    if (page) {
        page_get(page);  // Increment refcount
        page->age_score = 128;  // Start with medium age
        page->access_count = 1;
        page->last_access = get_ticks();
        
        // Set accessed flag since it's just allocated
        SetPageAccessed(page);
    }
    
    // Update mm_stats to match kalloc internal tracking
    mm_stats.free_pages = free_page_count;
    
    return pa;
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
     * MAX_PAGES = 32768, so bitmap = 4096 bytes = 1 page. */
    uint8_t free_bitmap[MAX_PAGES / 8];
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
        return (void *)base_pa;
    }

    return NULL; /* no contiguous 2MB block available */
}

// Free a 2MB contiguous block back to the free list.
// pa must be 2MB-aligned.
void kfree_super(void *pa) {
    uint64_t base_pa = (uint64_t)pa;
    if (!IS_SUPERPAGE_ALIGNED(base_pa))
        return;
    if (base_pa < 0x80000000UL || base_pa + SUPERPAGE_SIZE > PHYSTOP)
        return;
    int base_idx = pa_to_index(base_pa);

    /* Check base refcount -- only free when it reaches 0 */
    if (base_idx >= 0 && base_idx < MAX_PAGES) {
        if (page_refcount[base_idx] > 1) {
            page_refcount[base_idx]--;
            return; /* still referenced */
        }
        if (page_refcount[base_idx] == 0) {
            printk("WARNING: double kfree_super of pa 0x%lx! Ignoring.\n", (uint64_t)base_pa);
            return;
        }
    }

    /* Return all 512 constituent 4KB pages to the free list */
    for (int i = 0; i < SUPERPAGE_NPAGES; i++) {
        uint64_t sub_pa = base_pa + (uint64_t)i * PAGE_SIZE;
        int idx = pa_to_index(sub_pa);
        if (idx >= 0 && idx < MAX_PAGES)
            page_refcount[idx] = 0;
        struct run *r = (struct run *)sub_pa;
        r->next = kmem.freelist;
        kmem.freelist = r;
        free_page_count++;
    }
}


/* Enhanced kfree wrapper for page descriptor integration */

