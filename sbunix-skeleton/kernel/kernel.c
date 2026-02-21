#include <printk.h>
#include <uart.h>
#include <tar.h>
#include <kalloc.h>
#include <exec.h>
#include <vm.h>
#include <proc.h>

// Declare symbols for the embedded tarfs
extern char _tarfs_start[];
extern char _tarfs_end[];

void boot(void) {
  printk("Booting SBUnix\n");
  
  kinit();
  kvminit();
  
  procinit();
  userinit();

  printk("Starting scheduler...\n");
  scheduler();

  // Should never reach here
  while(1);
}