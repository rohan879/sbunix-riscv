#include <printk.h>
#include <riscv.h>

void panic(const char *msg) {
    /* Disable interrupts */
    w_sstatus(r_sstatus() & ~SSTATUS_SIE);

    printk("\n!!! KERNEL PANIC: %s !!!\n", msg);
    printk("sstatus=%p sepc=%p scause=%p stval=%p\n",
           r_sstatus(), r_sepc(), r_scause(), r_stval());

    /* Halt forever */
    for (;;)
        asm volatile("wfi");
}
