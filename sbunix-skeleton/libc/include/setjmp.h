#ifndef _SETJMP_H
#define _SETJMP_H

/* jmp_buf holds 14 callee-saved registers: ra, sp, s0-s11 */
typedef long jmp_buf[14];
typedef jmp_buf sigjmp_buf;

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#define sigsetjmp(env, savemask) ((void)(savemask), setjmp(env))

#endif /* _SETJMP_H */
