/*
 * setjmp_ext.c — siglongjmp implementation.
 *
 * Restores the signal mask (if it was saved by sigsetjmp) before
 * jumping back via longjmp.
 */

#include <setjmp.h>
#include <signal.h>

void siglongjmp(sigjmp_buf env, int val) {
  /* If sigsetjmp was called with savemask != 0, restore the signal mask */
  if (env->savemask) {
    sigprocmask(SIG_SETMASK, (const sigset_t *)&env->saved_mask, (sigset_t *)0);
  }
  /* Jump back to the sigsetjmp call site via the embedded jmp_buf */
  longjmp(env->buf, val);
}
