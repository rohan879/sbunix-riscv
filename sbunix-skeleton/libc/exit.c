#include <stdlib.h>
#include <syscall.h>

void exit(int status) {
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}
