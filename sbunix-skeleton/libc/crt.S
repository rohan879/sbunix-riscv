.global _start
.global environ

.section .data
environ: .quad 0

.section .text
_start:
    ld   a0, 0(sp)          # argc
    addi a1, sp, 8          # argv
    addi a2, a0, 1
    slli a2, a2, 3          # (argc+1)*8
    add  a2, a1, a2         # envp = argv + (argc+1)*8
    la   t0, environ
    sd   a2, 0(t0)          # store environ
    call main
    call exit
    j    .
