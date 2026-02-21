#include <kalloc.h>
#include <printk.h>
#include <riscv.h>

#define NULL ((void *)0)

extern char _kernel_end[]; // Symbol from linker script

struct run {
    struct run *next;
};

struct {
    struct run *freelist;
} kmem;

// Frees a page of physical memory.
void kfree(void *pa) {
    struct run *r;

    // For simplicity, we don't check for errors like freeing unaligned pages
    // or pages outside the valid range.

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
    }
    return r;
}