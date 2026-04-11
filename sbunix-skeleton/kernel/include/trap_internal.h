/* RF: Cross-module helpers shared between trap.c and syscall_*.c modules.
 * Part of the trap.c modular decomposition. */
#ifndef _TRAP_INTERNAL_H
#define _TRAP_INTERNAL_H

#include <proc.h>

#define TIMER_FREQ 10000000UL  /* 10 MHz timebase */

/* Time state (defined in trap.c) */
extern unsigned long boot_time;
extern unsigned long ticks;
extern uint64_t boot_epoch_ns;
extern unsigned char last_key;

/* Time helpers (defined in trap.c) */
unsigned long get_ticks(void);
void get_wall_time(int64_t *sec, int64_t *nsec);
uint64_t timespec_rel_deadline(uint64_t sec, uint64_t nsec);
uint64_t ns_to_time_ticks(uint64_t ns);

/* FD helpers (defined in trap.c) */
int alloc_fd(void);
void close_ofile(struct proc *p, int fd);
void copy_ofile(struct proc *dst, struct proc *src);

/* Process lifecycle (defined in syscall_process.c) */
void cleanup_proc_resources(struct proc *p);

/* VM support for fork (defined in syscall_process.c) */
int copy_pagetable_cow(pagetable_t dst, pagetable_t src,
                       uint64_t start, uint64_t end);

/* Signal (defined in trap.c) */
void deliver_signal(struct trapframe *tf, struct proc *p);
void kill_deliver(struct proc *target, int sig);

#endif
