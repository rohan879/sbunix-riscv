.section .text
.globl sigtrampoline
.globl sigtrampoline_end
sigtrampoline:
    /* a7 = SYS_rt_sigreturn (139) */
    li a7, 139
    ecall
sigtrampoline_end:
