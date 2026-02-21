#ifndef _TRAP_H
#define _TRAP_H

struct trapframe;

void trap_handler(struct trapframe *tf);

#endif
