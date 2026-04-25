#include <swap.h>
#include <string.h>

int swap_alloc(void) {
  return -1;
}

void swap_free(int slot) {
  (void)slot;
}

void swap_write(int slot, void *page) {
  (void)slot;
  (void)page;
}

void swap_read(int slot, void *page) {
  (void)slot;
  memset(page, 0, PAGE_SIZE);
}
