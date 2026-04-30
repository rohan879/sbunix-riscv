#include <user_clock.h>
#include <kalloc.h>
#include <string.h>
#include <vm.h>
#include <proc.h>

static void user_clock_publish(struct user_clock_state *uc,
                               uint64_t started_ticks,
                               int running) {
  struct user_clock_page *page;

  if (!uc || !uc->page)
    return;
  page = (struct user_clock_page *)uc->page;

  /* BF: libc reads this page without a syscall.  A sequence counter keeps
   * interrupt-time updates from exposing a mixed old/new clock snapshot. */
  page->seq++;
  page->accumulated_ticks = uc->accumulated_ticks;
  page->started_ticks = started_ticks;
  page->running = running ? 1 : 0;
  page->seq++;
}

struct user_clock_state *user_clock_create(void) {
  struct user_clock_state *uc;

  uc = (struct user_clock_state *)kalloc();
  if (!uc)
    return 0;
  memset(uc, 0, PAGE_SIZE);
  uc->page = kalloc();
  if (!uc->page) {
    kfree(uc);
    return 0;
  }
  memset(uc->page, 0, PAGE_SIZE);
  uc->refcount = 1;
  uc->accumulated_ticks = 0;
  user_clock_publish(uc, 0, 0);
  return uc;
}

void user_clock_get(struct user_clock_state *uc) {
  if (uc)
    uc->refcount++;
}

void user_clock_put(struct user_clock_state *uc) {
  if (!uc)
    return;
  uc->refcount--;
  if (uc->refcount > 0)
    return;
  if (uc->page)
    kfree(uc->page);
  kfree(uc);
}

int user_clock_map(pagetable_t pagetable, struct user_clock_state *uc) {
  if (!pagetable || !uc || !uc->page)
    return -1;
  return mappages(pagetable, USER_CLOCK_VA, PAGE_SIZE, (uint64_t)uc->page,
                  PTE_R | PTE_U);
}

void user_clock_unmap(pagetable_t pagetable) {
  pte_t *pte;

  if (!pagetable || pagetable == kernel_pagetable)
    return;
  pte = walk(pagetable, USER_CLOCK_VA, 0);
  if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
    /* BF: USER_CLOCK_VA's physical page is owned by user_clock_put(), not
     * generic user PTE teardown, so detach the mapping without kfree(). */
    *pte = 0;
    asm volatile("sfence.vma %0, zero" : : "r"(USER_CLOCK_VA) : "memory");
  }
}

void user_clock_reset_page(struct user_clock_state *uc) {
  if (!uc)
    return;
  uc->accumulated_ticks = 0;
  user_clock_publish(uc, 0, 0);
}

void user_clock_start(struct proc *p, uint64_t now) {
  if (!p || !p->user_clock)
    return;
  p->cpu_start_ticks = now;
  user_clock_publish(p->user_clock, now, 1);
}

void user_clock_update_running(struct proc *p, uint64_t now) {
  if (!p || !p->user_clock || p->cpu_start_ticks == 0)
    return;
  if (now >= p->cpu_start_ticks)
    p->user_clock->accumulated_ticks += now - p->cpu_start_ticks;
  p->cpu_start_ticks = now;
  user_clock_publish(p->user_clock, now, 1);
}

void user_clock_stop(struct proc *p, uint64_t now) {
  if (!p || !p->user_clock)
    return;
  if (p->cpu_start_ticks != 0 && now >= p->cpu_start_ticks)
    p->user_clock->accumulated_ticks += now - p->cpu_start_ticks;
  p->cpu_start_ticks = 0;
  user_clock_publish(p->user_clock, 0, 0);
}

uint64_t user_clock_current_ticks(struct proc *p, uint64_t now) {
  uint64_t ticks;

  if (!p || !p->user_clock)
    return 0;
  ticks = p->user_clock->accumulated_ticks;
  if (p->cpu_start_ticks != 0 && now >= p->cpu_start_ticks)
    ticks += now - p->cpu_start_ticks;
  return ticks;
}
