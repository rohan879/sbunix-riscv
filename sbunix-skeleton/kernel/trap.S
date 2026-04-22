.align 4
.global kernel_trap_handler
.global trap_restore

.extern trap_handler

.equ TF_SIZE, 272
.equ TF_T0, 8
.equ TF_T1, 16
.equ TF_T6, 56
.equ TF_SP, 240
.equ TF_EPC, 248
.equ TF_SSTATUS, 256
.equ TF_RETURN_TO_USER, 264
.equ SSTATUS_SPP_BIT, 0x100

kernel_trap_handler:
  # Swap sp with sscratch
  csrrw sp, sscratch, sp
  
  # If sp was 0, we were already in kernel mode.
  beqz sp, _trap_from_kernel

_trap_from_user:
  # sp now points to kernel stack, sscratch has user sp.
  # Allocate trapframe on kernel stack
  addi sp, sp, -TF_SIZE
  j _save_all

_trap_from_kernel:
  # Restore sp and save on current kernel stack
  csrrw sp, sscratch, sp
  addi sp, sp, -TF_SIZE

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
  addi t0, sp, TF_SIZE
_got_user_sp:
  sd t0, 240(sp)

  # Save sepc
  csrr t0, sepc
  sd t0, TF_EPC(sp)

  # Save trap provenance from hardware state.  SPP=0 means the trap was
  # taken from U-mode; trap_restore uses this instead of guessing from sp.
  csrr t0, sstatus
  sd t0, TF_SSTATUS(sp)
  li t1, SSTATUS_SPP_BIT
  and t0, t0, t1
  seqz t0, t0
  sd t0, TF_RETURN_TO_USER(sp)

  # Clear sscratch so nested kernel traps are detected correctly.
  # sscratch=0 means "in kernel mode" for kernel_trap_handler.
  csrw sscratch, zero

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

  # Restore sstatus/sepc from the saved trap provenance.
  ld t0, TF_SSTATUS(sp)
  csrw sstatus, t0

  # Restore sepc
  ld t0, TF_EPC(sp)
  csrw sepc, t0

  # Keep return mode in t6 until after the common register restore.
  ld t6, TF_RETURN_TO_USER(sp)

  # Restore registers.  Keep t0/t1/t6 until the final return path because
  # trap_restore needs scratch registers after the common restore.
  ld ra, 0(sp)
  ld t2, 24(sp)
  ld t3, 32(sp)
  ld t4, 40(sp)
  ld t5, 48(sp)
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

  beqz t6, _ret_kernel

_ret_user:
  # sscratch must hold the kernel stack top for the next user trap.
  addi t0, sp, TF_SIZE
  csrw sscratch, t0

  # Restore scratch registers, then atomically load the saved user sp using
  # the old trapframe base.  ld sp, offset(sp) uses the pre-write base.
  ld t6, TF_T6(sp)
  ld t1, TF_T1(sp)
  ld t0, TF_T0(sp)
  ld sp, TF_SP(sp)
  sret

_ret_kernel:
  ld t6, TF_T6(sp)
  ld t1, TF_T1(sp)
  ld t0, TF_T0(sp)
  addi sp, sp, TF_SIZE
  sret
