#ifndef _SYS_USER_CLOCK_H
#define _SYS_USER_CLOCK_H

#include <stdint.h>

#define USER_CLOCK_VA 0x3EFFF000UL
#define USER_CLOCK_TIMER_FREQ 10000000UL

struct user_clock_page {
  volatile uint64_t seq;
  volatile uint64_t accumulated_ticks;
  volatile uint64_t started_ticks;
  volatile uint64_t running;
};

#endif
