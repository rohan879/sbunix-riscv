#include <printk.h>
#include <uart.h>
#include <tar.h>
#include <kalloc.h>
#include <exec.h>
#include <vm.h>

// Declare symbols for the embedded tarfs
extern char _tarfs_start[];
extern char _tarfs_end[];

void boot(void) {
  printk("Booting SBUnix\n");
  
  kinit();
  kvminit();
  
  // Test the allocator
  void *p = kalloc();
  if (p) {
    printk("kalloc test: allocated a page at 0x%lx\n", (unsigned long)p);
    kfree(p);
    printk("kalloc test: freed the page.\n");
  } else {
    printk("kalloc test: failed to allocate a page.\n");
  }

  printk("Initializing embedded filesystem...\n");
  tar_ls(_tarfs_start, _tarfs_end);

  // Test exec for /bin/sbush
  printk("Attempting to exec /bin/sbush...\n");
  exec("./bin/sbush");

  // Re-enable echo service
  char c;
  printk("Echo service started. Type 'q' to exit.\n");
  while (1) {
    c = uart_read();
    if (c == 'q') {
      printk("\nExiting. Halting CPU.\n");
      break;
    }
    printk("%c", c);
  }
}
