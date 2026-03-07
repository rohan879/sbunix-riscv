#include <kmalloc.h>
#include <kalloc.h>
#include <string.h>

#define NBUCKETS 8
/* bucket sizes: 16, 32, 64, 128, 256, 512, 1024, 2048 */
static int bucket_size[NBUCKETS] = {16, 32, 64, 128, 256, 512, 1024, 2048};

struct kmalloc_node {
  struct kmalloc_node *next;
};

static struct kmalloc_node *freelists[NBUCKETS];

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
    return kalloc();
  }

  /* Check free list */
  if (freelists[b]) {
    struct kmalloc_node *node = freelists[b];
    freelists[b] = node->next;
    return (void *)node;
  }

  /* Allocate a new page and carve it into chunks */
  char *page = kalloc();
  if (!page)
    return 0;
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
    kfree(ptr);
    return;
  }

  struct kmalloc_node *node = (struct kmalloc_node *)ptr;
  node->next = freelists[b];
  freelists[b] = node;
}
