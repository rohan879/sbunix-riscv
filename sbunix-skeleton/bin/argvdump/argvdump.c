#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("argv[%d]=%s\n", i, argv[i] ? argv[i] : "(null)");

    char *env = getenv("SBX_EXEC_ENV");
    if (env)
        printf("env.SBX_EXEC_ENV=%s\n", env);
    else
        printf("env.SBX_EXEC_ENV=\n");

    return 0;
}
