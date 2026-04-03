#include <setjmp.h>

void siglongjmp(sigjmp_buf env, int val) {
  longjmp(env, val);
}
