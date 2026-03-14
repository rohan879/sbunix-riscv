/*
 * unametest — uname test
 *
 * Tests that uname() returns correct system information.
 */

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

int main(void)
{
    printf("=== unametest: uname ===\n");

    struct utsname buf;
    memset(&buf, 0, sizeof(buf));

    int r = uname(&buf);
    CHECK("uname: returns 0", r == 0);

    CHECK("uname: sysname is 'SBUnix'",
          strcmp(buf.sysname, "SBUnix") == 0);

    CHECK("uname: machine is 'riscv64'",
          strcmp(buf.machine, "riscv64") == 0);

    /* All fields should be non-empty strings */
    CHECK("uname: sysname non-empty", buf.sysname[0] != '\0');
    CHECK("uname: nodename non-empty", buf.nodename[0] != '\0');
    CHECK("uname: release non-empty", buf.release[0] != '\0');
    CHECK("uname: version non-empty", buf.version[0] != '\0');
    CHECK("uname: machine non-empty", buf.machine[0] != '\0');

    printf("=== unametest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
