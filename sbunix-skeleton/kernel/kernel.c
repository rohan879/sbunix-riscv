#include <printk.h>
#include <uart.h>
#include <tar.h>
#include <kalloc.h>
#include <exec.h>
#include <vm.h>
#include <proc.h>
#include <vfs.h>
#include <trap.h>
#include <riscv.h>
#include <plic.h>
#include <dtb.h>
#include <virtio_blk.h>

// Declare symbols for the embedded tarfs
extern char _tarfs_start[];
extern char _tarfs_end[];

void boot(uint64_t hartid, void *dtb) {
  printk("Booting SBUnix (hart %ld)\n", (long)hartid);

  /* Parse DTB before kinit — the DTB lives in memory that the page
     allocator will hand out once kinit runs. */
  dtb_parse(dtb);
  dtb_print_devices();

  kinit();
  kvminit();

  timer_init();
  plic_init();
  uart_intr_init();
  w_sie(r_sie() | SIE_STIE | SIE_SEIE);
  w_sstatus(r_sstatus() | SSTATUS_SIE);

  virtio_blk_test();

  procinit();
  vfs_init();
  userinit();

  printk("Starting scheduler...\n");
  scheduler();

  // Should never reach here
  while(1);
}