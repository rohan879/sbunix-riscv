.section .text.init
.global _start
_start:
  la sp, _stack_top
  
  # Initialize sscratch to 0
  csrw sscratch, zero

  la t0, kernel_trap_handler
  csrw stvec, t0
  call boot
  j .

_trap_handler:
  # Save t0 on the stack to prevent register corruption
  addi sp, sp, -8
  sd t0, 0(sp)

  # Acknowledge the breakpoint and return to the next instruction.
  csrr t0, sepc
  addi t0, t0, 4
  csrw sepc, t0

  # Restore t0
  ld t0, 0(sp)
  addi sp, sp, 8
  sret
