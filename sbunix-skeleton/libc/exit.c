#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>

#define ATEXIT_MAX 32

static void (*__atexit_funcs[ATEXIT_MAX])(void);
static int __atexit_count;

int atexit(void (*func)(void)) {
    if (__atexit_count >= ATEXIT_MAX)
        return -1;
    __atexit_funcs[__atexit_count++] = func;
    return 0;
}

void exit(int status) {
    /* Call atexit-registered functions in reverse order */
    for (int i = __atexit_count - 1; i >= 0; i--)
        __atexit_funcs[i]();
    fflush((void *)0);
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}
