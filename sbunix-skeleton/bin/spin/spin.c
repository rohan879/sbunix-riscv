#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>

int main(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: spin then print */
        for (volatile int i = 0; i < 5000000; i++);
        printf("child done\n");
        _exit(0);
    }
    /* Parent: spin then print */
    for (volatile int i = 0; i < 5000000; i++);
    printf("parent done\n");
    int status;
    waitpid(pid, &status, 0);
    return 0;
}
