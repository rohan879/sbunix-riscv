#include <kmalloc.h>
#include <kalloc.h>
#include <printk.h>
#include <string.h>

#define NBUCKETS 8
/* bucket sizes: 16, 32, 64, 128, 256, 512, 1024, 2048 */
static int bucket_size[NBUCKETS] = {16, 32, 64, 128, 256, 512, 1024, 2048};

/* BF: Keep slab ownership metadata aligned with the allocator's RAM size. */
#define MAX_PAGES (PHYSMEM_SIZE / PAGE_SIZE)
#define KM_PAGE_NONE 0
#define KM_PAGE_LARGE 0xFE

/*
 * Per-page ownership metadata for kmalloc pages.
 * 0: not managed by kmalloc
 * 1..NBUCKETS: slab page for bucket (value-1)
 * 0xFE: page returned by kmalloc(size > max bucket)
 */
static uint8_t km_page_owner[MAX_PAGES];
static int km_warn_count;

struct kmalloc_node {
  struct kmalloc_node *next;
};

static struct kmalloc_node *freelists[NBUCKETS];

static int km_pa_to_index(uint64_t pa) {
  return (int)((pa - 0x80000000UL) / PAGE_SIZE);
}

static int km_owner_get(void *ptr) {
  uint64_t page = PGROUNDDOWN((uint64_t)ptr);
  if (page < 0x80000000UL || page >= PHYSTOP)
    return KM_PAGE_NONE;
  int idx = km_pa_to_index(page);
  if (idx < 0 || idx >= MAX_PAGES)
    return KM_PAGE_NONE;
  return km_page_owner[idx];
}

int kmalloc_is_managed_page(void *pa) {
  return km_owner_get(pa) != KM_PAGE_NONE;
}

static void km_owner_set(void *page, uint8_t owner) {
  uint64_t pa = PGROUNDDOWN((uint64_t)page);
  if (pa < 0x80000000UL || pa >= PHYSTOP)
    return;
  int idx = km_pa_to_index(pa);
  if (idx < 0 || idx >= MAX_PAGES)
    return;
  km_page_owner[idx] = owner;
}

static void km_warn(const char *msg, void *ptr, uint64_t size) {
  if (km_warn_count < 64) {
    printk("kmalloc: %s ptr=0x%lx size=%lu\n", msg, (uint64_t)ptr, size);
    km_warn_count++;
    if (km_warn_count == 64)
      printk("kmalloc: too many warnings, suppressing further messages\n");
  }
}

static int size_to_bucket(uint64_t size) {
  for (int i = 0; i < NBUCKETS; i++)
    if (size <= (uint64_t)bucket_size[i])
      return i;
  return -1; /* too large -- use kalloc directly */
}

void *kmalloc(uint64_t size) {
  if (size == 0)
    return 0;

  int b = size_to_bucket(size);
  if (b < 0) {
    /* Large allocation: round up to pages */
    uint64_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    /* For simplicity, only support single-page large allocs */
    if (npages > 1)
      return 0;
    void *page = kalloc();
    if (page) {
      km_owner_set(page, KM_PAGE_LARGE);
      page_set_type((uint64_t)page, PAGE_SLAB);
    }
    return page;
  }

  /* Check free list */
  while (freelists[b]) {
    struct kmalloc_node *node = freelists[b];
    freelists[b] = node->next;
    if (km_owner_get(node) != (b + 1)) {
      km_warn("discarding corrupt freelist node", (void *)node, size);
      continue;
    }
    return (void *)node;
  }

  /* Allocate a new page and carve it into chunks */
  char *page = kalloc();
  if (!page)
    return 0;
  km_owner_set(page, (uint8_t)(b + 1));
  page_set_type((uint64_t)page, PAGE_SLAB);
  memset(page, 0, PAGE_SIZE);

  int chunk_size = bucket_size[b];
  int nchunks = PAGE_SIZE / chunk_size;

  /* First chunk is returned to caller */
  /* Remaining chunks go on free list */
  for (int i = 1; i < nchunks; i++) {
    struct kmalloc_node *node = (struct kmalloc_node *)(page + i * chunk_size);
    node->next = freelists[b];
    freelists[b] = node;
  }

  return (void *)page;
}

void kmfree(void *ptr, uint64_t size) {
  if (!ptr)
    return;

  int b = size_to_bucket(size);
  if (b < 0) {
    if (((uint64_t)ptr % PAGE_SIZE) != 0) {
      km_warn("reject non-page-aligned large free", ptr, size);
      return;
    }
    if (km_owner_get(ptr) != KM_PAGE_LARGE) {
      km_warn("reject non-large-page free", ptr, size);
      return;
    }
    km_owner_set(ptr, KM_PAGE_NONE);
    page_set_type((uint64_t)ptr, PAGE_UNKNOWN);
    kfree(ptr);
    return;
  }

  if (km_owner_get(ptr) != (b + 1)) {
    km_warn("reject wrong-bucket free", ptr, size);
    return;
  }

  uint64_t page = PGROUNDDOWN((uint64_t)ptr);
  uint64_t off = (uint64_t)ptr - page;
  if ((off % (uint64_t)bucket_size[b]) != 0) {
    km_warn("reject misaligned chunk free", ptr, size);
    return;
  }

  struct kmalloc_node *node = (struct kmalloc_node *)ptr;
  node->next = freelists[b];
  freelists[b] = node;
}
