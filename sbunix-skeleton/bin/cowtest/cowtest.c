#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] cow: %s\n", name); tests_passed++; }
    else { printf("[FAIL] cow: %s\n", name); tests_failed++; }
}

static int shared_data = 42;
static char big_buf[4096];

int main(void) {
    int status;
    pid_t pid;

    memset(big_buf, 'A', sizeof(big_buf));

    pid = fork();
    if (pid == 0) {
        if (shared_data != 42) exit(1);
        if (big_buf[0] != 'A') exit(2);
        shared_data = 99;
        big_buf[0] = 'B';
        exit(0);
    }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child sees parent data");
    check(shared_data == 42, "parent data unchanged");
    check(big_buf[0] == 'A', "parent buffer unchanged");

    /* multi-fork */
    for (int i = 0; i < 5; i++) {
        pid = fork();
        if (pid == 0) { shared_data = i; exit(i); }
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == i, "multi-fork child ok");
    }
    check(shared_data == 42, "parent intact after multi-fork");

    /* Heap-allocated COW: malloc buffer, fork, child writes */
    char *heap = malloc(4096);
    if (heap) {
        memset(heap, 'H', 4096);
        heap[0] = 'X';
        heap[4095] = 'Y';

        pid = fork();
        if (pid == 0) {
            /* Child: verify inherited data then overwrite */
            if (heap[0] != 'X' || heap[4095] != 'Y') exit(10);
            heap[0] = 'Z';    /* triggers COW fault on heap page */
            heap[2048] = 'W'; /* different page in large buffer */
            if (heap[0] != 'Z') exit(11);
            exit(0);
        }
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "heap COW: child wrote without crash");
        check(heap[0] == 'X', "heap COW: parent data unchanged");
        check(heap[4095] == 'Y', "heap COW: parent end unchanged");
        free(heap);
    }

    /* Stack COW: local variable, fork, child writes */
    {
        char stack_buf[256];
        memset(stack_buf, 'S', sizeof(stack_buf));
        pid = fork();
        if (pid == 0) {
            if (stack_buf[0] != 'S') exit(20);
            stack_buf[0] = 'T'; /* triggers COW on stack page */
            if (stack_buf[0] != 'T') exit(21);
            exit(0);
        }
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "stack COW: child wrote without crash");
        check(stack_buf[0] == 'S', "stack COW: parent unchanged");
    }

    /* Large heap COW: multiple pages */
    {
        int pages = 8;
        char *big = malloc(pages * 4096);
        if (big) {
            for (int i = 0; i < pages; i++)
                big[i * 4096] = 'A' + i;

            pid = fork();
            if (pid == 0) {
                /* Child writes to every page */
                for (int i = 0; i < pages; i++)
                    big[i * 4096] = 'a' + i;
                /* Verify child's writes */
                for (int i = 0; i < pages; i++)
                    if (big[i * 4096] != 'a' + i) exit(30 + i);
                exit(0);
            }
            waitpid(pid, &status, 0);
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "multi-page COW: child wrote all pages");
            /* Verify parent unchanged */
            int parent_ok = 1;
            for (int i = 0; i < pages; i++)
                if (big[i * 4096] != 'A' + i) parent_ok = 0;
            check(parent_ok, "multi-page COW: parent all pages intact");
            free(big);
        }
    }

    printf("cowtest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
