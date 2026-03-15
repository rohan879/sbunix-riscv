#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>

void exit(int status) {
    fflush((void *)0);
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}
