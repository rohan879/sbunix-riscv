# Trampoline page: mapped at the same VA in both user and kernel page tables.
# Contains trap entry (uservec) and trap exit (userret) code.
# Must be position-independent since it runs at TRAMPOLINE_VA, not its link address.

.section .text.trampoline
.globl trampoline
.globl uservec
.globl userret
.globl trampoline_end

.align 12  # page-aligned
trampoline:
uservec:
    # DEBUG: Write 'U' to UART (0x10000000) to prove we got here.
    # This works because the user PT still has kernel MMIO... wait, no.
    # The user PT has NO kernel MMIO.  We can't access UART here.
    # Instead, just proceed normally.

    # Trap from user mode.  sscratch holds the address of the process's
    # trapframe page (mapped at TRAPFRAME_VA in user space).
    # Swap a0 and sscratch so a0 = trapframe, sscratch = user a0.
    csrrw a0, sscratch, a0

    # Save all user registers into the trapframe (offsets match struct trapframe)
    sd ra, 0(a0)
    sd sp, 240(a0)   # user sp
    sd gp, 224(a0)
    sd tp, 232(a0)
    sd t0, 8(a0)
    sd t1, 16(a0)
    sd t2, 24(a0)
    sd t3, 32(a0)
    sd t4, 40(a0)
    sd t5, 48(a0)
    sd t6, 56(a0)
    sd a1, 72(a0)
    sd a2, 80(a0)
    sd a3, 88(a0)
    sd a4, 96(a0)
    sd a5, 104(a0)
    sd a6, 112(a0)
    sd a7, 120(a0)
    sd s0, 128(a0)
    sd s1, 136(a0)
    sd s2, 144(a0)
    sd s3, 152(a0)
    sd s4, 160(a0)
    sd s5, 168(a0)
    sd s6, 176(a0)
    sd s7, 184(a0)
    sd s8, 192(a0)
    sd s9, 200(a0)
    sd s10, 208(a0)
    sd s11, 216(a0)

    # Save the user a0 (currently in sscratch) into trapframe
    csrr t0, sscratch
    sd t0, 64(a0)   # a0 field

    # Save sepc (user PC)
    csrr t0, sepc
    sd t0, 248(a0)  # epc field

    # Load kernel values from the trapframe's scratch area.
    # The kernel stores these at known offsets ABOVE the 256-byte register area:
    #   trapframe + 256 = kernel_satp
    #   trapframe + 264 = kernel_sp (this process's kernel stack top)
    #   trapframe + 272 = kernel_trap (address of trap_handler in kernel)
    #   trapframe + 280 = kernel_trapframe_pa (physical addr of this trapframe)
    ld sp, 264(a0)     # kernel sp
    ld t0, 256(a0)     # kernel satp
    ld t1, 272(a0)     # kernel trap handler address
    ld a0, 280(a0)     # a0 = physical trapframe addr (for trap_handler argument)

    # Switch to kernel page table
    csrw satp, t0
    sfence.vma zero, zero

    # Jump to the kernel trap handler with a0 = trapframe physical addr.
    # trap_handler does NOT return — it calls usertrapret() which
    # jumps to the userret trampoline to return to user mode.
    jr t1

    .align 4
userret:
    # Called from kernel with:
    #   a0 = TRAPFRAME_VA (user-space address of this process's trapframe)
    #   a1 = user page table satp value
    # We are currently running on kernel page table.
    # Switch to user page table, then restore registers and sret.

    csrw satp, a1
    sfence.vma zero, zero

    # Now running on user page table. a0 = TRAPFRAME_VA.

    # Restore user registers from trapframe
    ld ra, 0(a0)
    ld sp, 240(a0)
    ld gp, 224(a0)
    ld tp, 232(a0)
    ld t0, 8(a0)
    ld t1, 16(a0)
    ld t2, 24(a0)
    ld t3, 32(a0)
    ld t4, 40(a0)
    ld t5, 48(a0)
    ld t6, 56(a0)
    # skip a0 — restore it last
    ld a1, 72(a0)
    ld a2, 80(a0)
    ld a3, 88(a0)
    ld a4, 96(a0)
    ld a5, 104(a0)
    ld a6, 112(a0)
    ld a7, 120(a0)
    ld s0, 128(a0)
    ld s1, 136(a0)
    ld s2, 144(a0)
    ld s3, 152(a0)
    ld s4, 160(a0)
    ld s5, 168(a0)
    ld s6, 176(a0)
    ld s7, 184(a0)
    ld s8, 192(a0)
    ld s9, 200(a0)
    ld s10, 208(a0)
    ld s11, 216(a0)

    # Restore sepc
    ld t0, 248(a0)
    csrw sepc, t0

    # Restore a0 last
    ld a0, 64(a0)

    sret

trampoline_end:
