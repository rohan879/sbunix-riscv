#ifndef _USER_CLOCK_H
#define _USER_CLOCK_H

#include <riscv.h>

struct proc;

struct user_clock_page {
  volatile uint64_t seq;
  volatile uint64_t accumulated_ticks;
  volatile uint64_t started_ticks;
  volatile uint64_t running;
};

struct user_clock_state {
  void *page;
  int refcount;
  uint64_t accumulated_ticks;
};

struct user_clock_state *user_clock_create(void);
void user_clock_get(struct user_clock_state *uc);
void user_clock_put(struct user_clock_state *uc);
int user_clock_map(pagetable_t pagetable, struct user_clock_state *uc);
void user_clock_unmap(pagetable_t pagetable);
void user_clock_start(struct proc *p, uint64_t now);
void user_clock_update_running(struct proc *p, uint64_t now);
void user_clock_stop(struct proc *p, uint64_t now);
uint64_t user_clock_current_ticks(struct proc *p, uint64_t now);
void user_clock_reset_page(struct user_clock_state *uc);

#endif
