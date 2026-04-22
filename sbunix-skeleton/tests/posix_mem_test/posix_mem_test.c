#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <string.h>

#define DIRTY_PAGES 64

static int dirty_and_exit(int round) {
    size_t len = DIRTY_PAGES * PAGE_SIZE;
    unsigned char *region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        return 10 + round;

    for (size_t i = 0; i < len; i++)
        region[i] = (unsigned char)(0x40 + round);

    {
        char stack_buf[PAGE_SIZE];
        memset(stack_buf, 0x90 + round, sizeof(stack_buf));
        if (stack_buf[0] == 0)
            return 20 + round;
    }

    return 0;
}

int main(void) {
    int status;
    int pfd[2];
    char out[128];
    ssize_t nr;

    printf("posix_mem_test: dirty freed pages, then exec /bin/echo\n");

    for (int round = 0; round < 6; round++) {
        pid_t child = fork();
        if (child < 0) {
            printf("[FAIL] posix_mem_test: fork dirty child\n");
            return 1;
        }
        if (child == 0)
            _exit(dirty_and_exit(round));

        waitpid(child, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("[FAIL] posix_mem_test: dirty child round %d status=0x%x\n",
                   round, status);
            return 1;
        }
    }

    if (pipe(pfd) != 0) {
        printf("[FAIL] posix_mem_test: pipe\n");
        return 1;
    }

    pid_t exec_child = fork();
    if (exec_child < 0) {
        printf("[FAIL] posix_mem_test: fork exec child\n");
        return 1;
    }
    if (exec_child == 0) {
        char *argv[] = {"echo", "exec-zero-ok", NULL};
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        execv("/bin/echo", argv);
        _exit(127);
    }

    close(pfd[1]);
    nr = read(pfd[0], out, sizeof(out) - 1);
    close(pfd[0]);
    if (nr < 0)
        nr = 0;
    out[nr] = '\0';

    waitpid(exec_child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("[FAIL] posix_mem_test: exec child status=0x%x\n", status);
        return 1;
    }
    if (strcmp(out, "exec-zero-ok\n") != 0) {
        printf("[FAIL] posix_mem_test: unexpected output '%s'\n", out);
        return 1;
    }

    printf("[PASS] posix_mem_test\n");
    return 0;
}
