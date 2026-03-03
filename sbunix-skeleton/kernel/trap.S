.align 4
.global kernel_trap_handler
.global trap_restore

.extern trap_handler

kernel_trap_handler:
  # Swap sp with sscratch
  csrrw sp, sscratch, sp
  
  # If sp was 0, we were already in kernel mode.
  beqz sp, _trap_from_kernel

_trap_from_user:
  # sp now points to kernel stack, sscratch has user sp.
  # Allocate trapframe on kernel stack
  addi sp, sp, -256
  j _save_all

_trap_from_kernel:
  # Restore sp and save on current kernel stack
  csrrw sp, sscratch, sp
  addi sp, sp, -256

_save_all:
  # Save all registers
  sd ra, 0(sp)
  sd t0, 8(sp)
  sd t1, 16(sp)
  sd t2, 24(sp)
  sd t3, 32(sp)
  sd t4, 40(sp)
  sd t5, 48(sp)
  sd t6, 56(sp)
  sd a0, 64(sp)
  sd a1, 72(sp)
  sd a2, 80(sp)
  sd a3, 88(sp)
  sd a4, 96(sp)
  sd a5, 104(sp)
  sd a6, 112(sp)
  sd a7, 120(sp)
  sd s0, 128(sp)
  sd s1, 136(sp)
  sd s2, 144(sp)
  sd s3, 152(sp)
  sd s4, 160(sp)
  sd s5, 168(sp)
  sd s6, 176(sp)
  sd s7, 184(sp)
  sd s8, 192(sp)
  sd s9, 200(sp)
  sd s10, 208(sp)
  sd s11, 216(sp)
  sd gp, 224(sp)
  sd tp, 232(sp)

  # Save the original sp
  csrr t0, sscratch
  bnez t0, _got_user_sp
  addi t0, sp, 256
_got_user_sp:
  sd t0, 240(sp)

  # Save sepc
  csrr t0, sepc
  sd t0, 248(sp)

  # Call C handler
  mv a0, sp
  call trap_handler

  # After trap_handler returns, jump to restore
  mv a0, sp
  j trap_restore

  # --- Restore ---
trap_restore:
  # a0 points to the trapframe
  mv sp, a0

  # Restore sepc
  ld t0, 248(sp)
  csrw sepc, t0

  # Restore registers
  ld ra, 0(sp)
  ld t0, 8(sp)
  ld t1, 16(sp)
  ld t2, 24(sp)
  ld t3, 32(sp)
  ld t4, 40(sp)
  ld t5, 48(sp)
  ld t6, 56(sp)
  ld a0, 64(sp)
  ld a1, 72(sp)
  ld a2, 80(sp)
  ld a3, 88(sp)
  ld a4, 96(sp)
  ld a5, 104(sp)
  ld a6, 112(sp)
  ld a7, 120(sp)
  ld s0, 128(sp)
  ld s1, 136(sp)
  ld s2, 144(sp)
  ld s3, 152(sp)
  ld s4, 160(sp)
  ld s5, 168(sp)
  ld s6, 176(sp)
  ld s7, 184(sp)
  ld s8, 192(sp)
  ld s9, 200(sp)
  ld s10, 208(sp)
  ld s11, 216(sp)
  ld gp, 224(sp)
  ld tp, 232(sp)

  # Final stack restore
  ld t0, 240(sp)
  li t1, 0x80000000
  blt t0, t1, _ret_user

_ret_kernel:
  addi sp, sp, 256
  sret

_ret_user:
  # Restore sscratch and swap
  csrw sscratch, t0
  addi sp, sp, 256
  csrrw sp, sscratch, sp
  sret
