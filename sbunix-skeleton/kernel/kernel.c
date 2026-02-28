#include <printk.h>
#include <uart.h>
#include <tar.h>
#include <kalloc.h>
#include <exec.h>
#include <vm.h>
#include <proc.h>
#include <trap.h>
#include <riscv.h>

// Declare symbols for the embedded tarfs
extern char _tarfs_start[];
extern char _tarfs_end[];

void boot(void) {
  printk("Booting SBUnix\n");
  
  kinit();
  kvminit();

  timer_init();
  w_sie(r_sie() | SIE_STIE);
  w_sstatus(r_sstatus() | SSTATUS_SIE);

  procinit();
  userinit();

  printk("Starting scheduler...\n");
  scheduler();

  // Should never reach here
  while(1);
}