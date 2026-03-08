#ifndef _TRAP_H
#define _TRAP_H

struct trapframe;

void trap_handler(struct trapframe *tf);
void timer_init(void);
void syscall_init(void);

#endif
