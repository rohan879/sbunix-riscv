/* RF: Cross-module helpers shared between trap.c and syscall_*.c modules.
 * Part of the trap.c modular decomposition. */
#ifndef _TRAP_INTERNAL_H
#define _TRAP_INTERNAL_H

#include <proc.h>
#include <timekeeping.h>

/* Trap/timer state (defined in trap.c) */
extern unsigned long ticks;
extern unsigned char last_key;

/* Timer helpers */
unsigned long get_ticks(void);

/* MAP_SHARED writeback (defined in syscall_memory.c) */
void mmap_writeback_shared(struct proc *p, uint64_t start, uint64_t end);

/* FD helpers (defined in trap.c) */
int alloc_fd(void);
void close_ofile(struct proc *p, int fd);
void copy_ofile(struct proc *dst, struct proc *src);

/* Process lifecycle (defined in syscall_process.c) */
void cleanup_proc_resources(struct proc *p);
void release_vfork_parent(struct proc *child);
void proc_exit_current(int status, int terminate_signal);
void proc_force_terminate_wake(struct proc *target, int sig);

/* VM support for fork (defined in syscall_process.c) */
int copy_pagetable_cow(pagetable_t dst, pagetable_t src,
                       uint64_t start, uint64_t end);

/* Signal (defined in trap.c) */
void deliver_signal(struct trapframe *tf, struct proc *p);
void kill_deliver(struct proc *target, int sig);

#endif
