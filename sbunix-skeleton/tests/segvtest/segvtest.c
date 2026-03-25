#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static void handler(int sig) {
    (void)sig;
    printf("SIGSEGV caught\n");
    _exit(42);
}

int main(void) {
    struct sigaction sa;
    sa.sa_handler = handler;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sa.sa_restorer = 0;
    sigaction(SIGSEGV, &sa, 0);

    printf("dereferencing NULL...\n");
    *(volatile int *)0 = 0xDEAD;

    /* Should never reach here */
    printf("NOT REACHED\n");
    return 1;
}
