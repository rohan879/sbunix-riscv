#ifndef _SETJMP_H
#define _SETJMP_H

/* jmp_buf holds 14 callee-saved registers: ra, sp, s0-s11 */
typedef long jmp_buf[14];

/*
 * sigjmp_buf layout — extends jmp_buf with signal mask state.
 * Offsets (bytes):
 *   0..111  : jmp_buf (14 longs = 112 bytes)
 *   112     : saved_mask (unsigned long — the old signal mask)
 *   120     : savemask   (int — nonzero if mask was saved)
 *
 * sigsetjmp MUST be implemented in assembly because a C wrapper would
 * record the wrong return address (ra).
 */
#define SIGJMP_BUF_OFF   0
#define SIGJMP_MASK_OFF  112  /* 14 * sizeof(long) */
#define SIGJMP_FLAG_OFF  120

typedef struct {
  jmp_buf buf;
  unsigned long saved_mask;
  int savemask;
} sigjmp_buf_t;

typedef sigjmp_buf_t sigjmp_buf[1];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* sigsetjmp implemented in setjmp.S — saves regs + optionally signal mask */
int  sigsetjmp(sigjmp_buf env, int savemask);
/* siglongjmp implemented in setjmp_ext.c — restores mask then longjmp */
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#endif /* _SETJMP_H */
