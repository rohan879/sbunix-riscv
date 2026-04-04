.section .text
.globl sigtrampoline
.globl sigtrampoline_end

#ifdef __CHERI_PURE_CAPABILITY__
.option push
.option nocapmode
#endif

sigtrampoline:
    /* a7 = SYS_rt_sigreturn (139) */
    li a7, 139
    ecall
sigtrampoline_end:

#ifdef __CHERI_PURE_CAPABILITY__
.option pop
#endif
