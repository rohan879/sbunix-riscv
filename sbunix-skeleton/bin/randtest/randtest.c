/*
 * randtest — verify getrandom syscall works correctly.
 *
 * Tests:
 *  1. getrandom returns the requested byte count
 *  2. Output buffer is actually written (not all zeros)
 *  3. Two consecutive calls produce different output
 */

#include <stdio.h>
#include <syscall.h>
#include <string.h>

int main(void) {
    unsigned char buf1[32];
    unsigned char buf2[32];
    int pass = 0;
    int fail = 0;

    /* Zero the buffers so we can detect writes */
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));

    /* Test 1: return value equals requested length */
    long ret = syscall(SYS_getrandom, buf1, sizeof(buf1), 0);
    if (ret == (long)sizeof(buf1)) {
        printf("  PASS  getrandom returns correct byte count (%ld)\n", ret);
        pass++;
    } else {
        printf("  FAIL  getrandom returned %ld, expected %ld\n", ret, (long)sizeof(buf1));
        fail++;
    }

    /* Test 2: buffer is not all zeros */
    int all_zero = 1;
    for (int i = 0; i < 32; i++) {
        if (buf1[i] != 0) { all_zero = 0; break; }
    }
    if (!all_zero) {
        printf("  PASS  getrandom produced non-zero output\n");
        pass++;
    } else {
        printf("  FAIL  getrandom buffer is all zeros\n");
        fail++;
    }

    /* Test 3: two calls produce different output */
    ret = syscall(SYS_getrandom, buf2, sizeof(buf2), 0);
    if (ret == (long)sizeof(buf2)) {
        if (memcmp(buf1, buf2, 32) != 0) {
            printf("  PASS  two getrandom calls produced different output\n");
            pass++;
        } else {
            printf("  FAIL  two getrandom calls produced identical output\n");
            fail++;
        }
    } else {
        printf("  FAIL  second getrandom returned %ld\n", ret);
        fail++;
    }

    /* Test 4: small request (1 byte) */
    unsigned char one = 0;
    ret = syscall(SYS_getrandom, &one, 1, 0);
    if (ret == 1) {
        printf("  PASS  getrandom 1-byte request works\n");
        pass++;
    } else {
        printf("  FAIL  getrandom 1-byte returned %ld\n", ret);
        fail++;
    }

    /* Test 5: zero-length request */
    ret = syscall(SYS_getrandom, buf1, 0, 0);
    if (ret == 0) {
        printf("  PASS  getrandom 0-byte request returns 0\n");
        pass++;
    } else {
        printf("  FAIL  getrandom 0-byte returned %ld\n", ret);
        fail++;
    }

    printf("\n=== randtest: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
