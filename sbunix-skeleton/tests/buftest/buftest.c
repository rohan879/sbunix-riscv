#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    for (int i = 0; i < 100; i++)
        printf("line %d: hello from buffered output\n", i);
    fprintf(stderr, "stderr: this should appear immediately\n");
    printf("before flush...");
    fflush(stdout);
    printf(" after flush\n");
    return 0;
}
