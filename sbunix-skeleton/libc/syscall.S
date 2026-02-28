.global syscall
syscall:
    mv a7, a0      # syscall number -> a7
    mv a0, a1      # shift args down
    mv a1, a2
    mv a2, a3
    mv a3, a4
    mv a4, a5
    mv a5, a6
    ecall
    ret
