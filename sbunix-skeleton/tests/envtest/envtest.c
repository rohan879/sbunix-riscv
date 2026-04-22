/*
 * envtest — environment variable test
 *
 * Tests getenv, setenv, unsetenv, and environ pointer.
 *
 * Run from the shell:  envtest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] envtest: %s\n", label); g_pass++; } \
    else      { printf("[FAIL] envtest: %s\n", label); g_fail++; } \
} while (0)

int main(void) {
  printf("=== envtest: environment variable test ===\n");

  /* Test 1: environ is not NULL */
  CHECK("environ != NULL", environ != NULL);

  /* Test 2: getenv("PATH") — may or may not be set, but should not crash */
  {
    char *path = getenv("PATH");
    /* PATH might not be set in this OS — just verify no crash */
    CHECK("getenv(PATH) does not crash", 1);
    (void)path;
  }

  /* Test 3: setenv new variable */
  {
    int r = setenv("ENVTEST_VAR", "hello", 0);
    CHECK("setenv new variable returns 0", r == 0);
    char *v = getenv("ENVTEST_VAR");
    CHECK("getenv after setenv returns correct value",
          v != NULL && strcmp(v, "hello") == 0);
  }

  /* Test 4: setenv overwrite */
  {
    int r = setenv("ENVTEST_VAR", "world", 1);
    CHECK("setenv overwrite returns 0", r == 0);
    char *v = getenv("ENVTEST_VAR");
    CHECK("getenv after overwrite returns new value",
          v != NULL && strcmp(v, "world") == 0);
  }

  /* Test 5: setenv no-overwrite */
  {
    int r = setenv("ENVTEST_VAR", "nope", 0);
    CHECK("setenv no-overwrite returns 0", r == 0);
    char *v = getenv("ENVTEST_VAR");
    CHECK("getenv after no-overwrite keeps old value",
          v != NULL && strcmp(v, "world") == 0);
  }

  /* Test 6: unsetenv */
  {
    int r = unsetenv("ENVTEST_VAR");
    CHECK("unsetenv returns 0", r == 0);
    char *v = getenv("ENVTEST_VAR");
    CHECK("getenv after unsetenv returns NULL", v == NULL);
  }

  printf("envtest: %d passed, %d failed\n", g_pass, g_fail);
  return (g_fail > 0) ? 1 : 0;
}
