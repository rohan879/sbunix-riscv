/*
 * setjmp.S — setjmp/longjmp/sigsetjmp for RISC-V 64-bit.
 *
 * jmp_buf layout (14 longs): ra, sp, s0, s1, s2, ..., s11
 *
 * sigjmp_buf layout:
 *   +0   .. +111  : jmp_buf  (14 longs = 112 bytes)
 *   +112          : saved_mask (unsigned long)
 *   +120          : savemask flag (int)
 */

.global setjmp
.type setjmp, @function
setjmp:
    sd ra,   0(a0)
    sd sp,   8(a0)
    sd s0,  16(a0)
    sd s1,  24(a0)
    sd s2,  32(a0)
    sd s3,  40(a0)
    sd s4,  48(a0)
    sd s5,  56(a0)
    sd s6,  64(a0)
    sd s7,  72(a0)
    sd s8,  80(a0)
    sd s9,  88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)
    li a0, 0
    ret

.global longjmp
.type longjmp, @function
longjmp:
    ld ra,   0(a0)
    ld sp,   8(a0)
    ld s0,  16(a0)
    ld s1,  24(a0)
    ld s2,  32(a0)
    ld s3,  40(a0)
    ld s4,  48(a0)
    ld s5,  56(a0)
    ld s6,  64(a0)
    ld s7,  72(a0)
    ld s8,  80(a0)
    ld s9,  88(a0)
    ld s10, 96(a0)
    ld s11, 104(a0)
    /* Return val; if val == 0, return 1 instead */
    mv a0, a1
    bnez a0, 1f
    li a0, 1
1:
    ret

/*
 * sigsetjmp(sigjmp_buf env, int savemask)
 *
 * Must be in assembly because a C wrapper would record the wrapper's ra
 * instead of the caller's.  Steps:
 *   1. Save callee-saved regs (ra, sp, s0-s11) into env->buf
 *   2. If savemask == 0: store flag=0, return 0
 *   3. If savemask != 0: preserve env in s0, call
 *      sigprocmask(SIG_BLOCK=0, NULL, &env->saved_mask),
 *      store flag=1, RESTORE ra from env->buf (call clobbered it), return 0
 */
.global sigsetjmp
.type sigsetjmp, @function
sigsetjmp:
    /* Step 1: save all callee-saved regs into env->buf (offset 0) */
    sd ra,   0(a0)
    sd sp,   8(a0)
    sd s0,  16(a0)
    sd s1,  24(a0)
    sd s2,  32(a0)
    sd s3,  40(a0)
    sd s4,  48(a0)
    sd s5,  56(a0)
    sd s6,  64(a0)
    sd s7,  72(a0)
    sd s8,  80(a0)
    sd s9,  88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)

    /* Step 2: check savemask (in a1) */
    bnez a1, .Lsave_mask

    /* savemask == 0: store flag=0, return 0 */
    sw zero, 120(a0)       /* env->savemask = 0 */
    li a0, 0
    ret

.Lsave_mask:
    /* savemask != 0: need to call sigprocmask to retrieve current mask */
    /* Save env pointer in callee-saved s0 (we already saved old s0) */
    mv s0, a0

    /* sigprocmask(SIG_BLOCK=0, NULL, &env->saved_mask) */
    /*   a0 = how = 0 (SIG_BLOCK) */
    /*   a1 = set = NULL */
    /*   a2 = oldset = &env->saved_mask (offset 112) */
    li a0, 0               /* SIG_BLOCK */
    li a1, 0               /* NULL — don't change mask */
    addi a2, s0, 112       /* &env->saved_mask */
    call sigprocmask

    /* Store flag=1 to mark that mask was saved */
    li t0, 1
    sw t0, 120(s0)         /* env->savemask = 1 */

    /* Restore ra from env->buf — call to sigprocmask clobbered it */
    ld ra, 0(s0)
    /* Restore s0 — we clobbered it to hold env */
    ld s0, 16(s0)

    /* Return 0 (first return from sigsetjmp) */
    li a0, 0
    ret
