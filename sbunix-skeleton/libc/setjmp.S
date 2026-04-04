/*
 * setjmp.S — setjmp/longjmp for RISC-V 64-bit.
 *
 * jmp_buf layout (14 longs): ra, sp, s0, s1, s2, ..., s11
 */

.global setjmp
.type setjmp, @function

#ifdef __CHERI_PURE_CAPABILITY__
.option push
.option nocapmode
#endif

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

#ifdef __CHERI_PURE_CAPABILITY__
.option pop
#endif
