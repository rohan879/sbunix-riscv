#ifndef _TIMEKEEPING_H
#define _TIMEKEEPING_H

#include <riscv.h>

#define TIMER_FREQ 10000000UL
#define TIMER_HZ 100UL
#define TIMER_INTERVAL_TICKS (TIMER_FREQ / TIMER_HZ)
#define NSEC_PER_SEC 1000000000ULL
#define USEC_PER_SEC 1000000ULL
#define CLK_TCK 100ULL

extern unsigned long boot_time;
extern uint64_t boot_epoch_ns;

void timekeeping_init(uint64_t boot_ticks, uint64_t epoch_ns, int realtime_valid);
int timekeeping_realtime_available(void);
uint64_t timekeeping_elapsed_ticks(void);
uint64_t ns_to_time_ticks(uint64_t ns);
uint64_t timespec_to_ticks(uint64_t sec, uint64_t nsec);
uint64_t seconds_to_ticks(uint64_t sec);
uint64_t ticks_to_ns(uint64_t ticks);
void ticks_to_timespec(uint64_t ticks, int64_t *sec, int64_t *nsec);
void ticks_to_timeval(uint64_t ticks, int64_t *sec, int64_t *usec);
uint64_t timespec_rel_deadline(uint64_t sec, uint64_t nsec);
int get_wall_time(int64_t *sec, int64_t *nsec);
uint64_t timekeeping_now_seconds(void);
uint64_t timekeeping_to_clock_ticks(uint64_t ticks);

#endif
