#include <kalloc.h>
#include <printk.h>
#include <riscv.h>

extern char _kernel_end[]; // Symbol from linker script

struct run {
    struct run *next;
};

struct {
    struct run *freelist;
} kmem;

/* Reference counts for physical pages */
#define MAX_PAGES ((128 * 1024 * 1024) / PAGE_SIZE) /* 128MB / 4KB = 32768 */
static int page_refcount[MAX_PAGES];

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

// Frees a page of physical memory.
// Only actually frees when refcount reaches 0.
void kfree(void *pa) {
    struct run *r;

    int idx = pa_to_index((uint64_t)pa);
    if (idx >= 0 && idx < MAX_PAGES) {
        if (page_refcount[idx] > 1) {
            page_refcount[idx]--;
            return; /* still referenced */
        }
        page_refcount[idx] = 0;
    }

    r = (struct run*)pa;
    r->next = kmem.freelist;
    kmem.freelist = r;
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
        kfree(p);
    }

    printk("kinit: memory initialized.\n");
}

// Allocates one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns NULL if the memory cannot be allocated.
void *kalloc(void) {
    struct run *r;

    r = kmem.freelist;
    if(r) {
        kmem.freelist = r->next;
        page_refcount[pa_to_index((uint64_t)r)] = 1;
    }
    return r;
}
