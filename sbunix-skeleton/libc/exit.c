#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>

#define ATEXIT_MAX 32

static void (*_atexit_funcs[ATEXIT_MAX])(void);
static int _atexit_count;

int atexit(void (*func)(void)) {
    if (!func || _atexit_count >= ATEXIT_MAX)
        return -1;
    _atexit_funcs[_atexit_count++] = func;
    return 0;
}

void exit(int status) {
    /* Call atexit handlers in LIFO order */
    while (_atexit_count > 0)
        _atexit_funcs[--_atexit_count]();

    fflush((void *)0);
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}
