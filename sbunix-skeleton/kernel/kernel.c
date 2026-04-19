#include <printk.h>
#include <uart.h>
#include <tar.h>
#include <kalloc.h>
#include <exec.h>
#include <vm.h>
#include <mm.h>
#include <proc.h>
#include <vfs.h>
#include <trap.h>
#include <riscv.h>
#include <plic.h>
#include <dtb.h>
#include <virtio_blk.h>
#include <ext2.h>
#include <tty.h>
#include <swap.h>

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
  mm_init_page_descriptors(); /* BF: page descriptors must be initialized before
                                 any virt_to_page() or page accounting is used */
  mm_init_watermarks();       /* BF: set reclaim watermarks based on total pages */
  anon_vma_init();  /* Initialize anonymous VMA subsystem */
  kvminit();

  timer_init();
  plic_init();
  uart_intr_init();
  tty_init();
  w_sie(r_sie() | SIE_STIE | SIE_SEIE);
  w_sstatus(r_sstatus() | SSTATUS_SIE);

  if (virtio_blk_init() == 0) {
    ext2_init();
    swap_init();
  }

  procinit();
  vfs_init();
  syscall_init();
  userinit();

  /* Verify kernel code PTE integrity and W^X policy before scheduling. */
  if (recursive_check_kcode_pte())
    printk("boot: repaired kernel code PTE permissions\n");
  int wx_bad = kernel_wx_audit();
  if (wx_bad == 0)
    printk("boot: kernel W^X verified\n");
  else
    printk("boot: WARNING kernel W^X audit found %d bad mapping(s)\n", wx_bad);
  /* Verify the self-referencing window is functional by reading a
   * known L2 entry through the window and comparing to direct access */
  {
    volatile pte_t *l2r =
      (volatile pte_t *)RECURSIVE_L2_PTE_VA(2);
    if (*l2r == kernel_pagetable[2])
      printk("boot: recursive paging verified\n");
    else
      printk("boot: WARNING recursive paging mismatch\n");
  }

  printk("Starting scheduler...\n");
  scheduler();

  // Should never reach here
  while(1);
}
