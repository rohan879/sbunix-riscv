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

  /* Workaround: something during boot corrupts the kernel code PTE
   * (at 0x87ffa008), clearing PTE_X.  This causes instruction page
   * faults when the kernel executes code.  Restore PTE_X before
   * starting the scheduler so user processes run on a correct page
   * table.  Root cause TBD — GDB shows corruption during
   * virtio_blk_init but virtqueue pages don't overlap with the
   * page table page and it is pinned. */
  {
    pte_t l2pte = kernel_pagetable[2]; /* VPN[2]=2 for 0x80000000+ */
    if (l2pte & PTE_V) {
      pagetable_t l1 = (pagetable_t)PTE2PA(l2pte);
      if ((l1[0] & PTE_V) && !(l1[0] & PTE_X)) {
        l1[0] |= PTE_X;
        asm volatile("sfence.vma zero, zero");
      }
    }
  }

  printk("Starting scheduler...\n");
  scheduler();

  // Should never reach here
  while(1);
}