#include <stdio.h>
#include <signal.h>
#include <unistd.h>

volatile sig_atomic_t got_alarm = 0;

static void handler(int sig) {
    (void)sig;
    got_alarm = 1;
}

int main(void) {
    struct sigaction sa;
    sa.sa_handler = handler;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sa.sa_restorer = 0;
    sigaction(SIGALRM, &sa, 0);

    alarm(2);
    printf("waiting for alarm...\n");
    while (!got_alarm);
    printf("SIGALRM received\n");
    return 0;
}
