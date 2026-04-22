#include <timekeeping.h>

#define UINT64_MAX_LOCAL (~(uint64_t)0)

unsigned long boot_time;
uint64_t boot_epoch_ns;

static int realtime_available_value;

static uint64_t saturating_add(uint64_t a, uint64_t b) {
  if (UINT64_MAX_LOCAL - a < b)
    return UINT64_MAX_LOCAL;
  return a + b;
}

static uint64_t saturating_mul(uint64_t a, uint64_t b) {
  if (a != 0 && b > UINT64_MAX_LOCAL / a)
    return UINT64_MAX_LOCAL;
  return a * b;
}

void timekeeping_init(uint64_t boot_ticks, uint64_t epoch_ns, int realtime_valid) {
  boot_time = boot_ticks;
  boot_epoch_ns = epoch_ns;
  realtime_available_value = realtime_valid;
}

int timekeeping_realtime_available(void) {
  return realtime_available_value;
}

uint64_t timekeeping_elapsed_ticks(void) {
  return r_time() - boot_time;
}

uint64_t ticks_to_ns(uint64_t ticks) {
  uint64_t sec = ticks / TIMER_FREQ;
  uint64_t rem = ticks % TIMER_FREQ;
  uint64_t ns = saturating_mul(sec, NSEC_PER_SEC);
  return saturating_add(ns, (rem * NSEC_PER_SEC) / TIMER_FREQ);
}

void ticks_to_timespec(uint64_t ticks, int64_t *sec, int64_t *nsec) {
  if (sec)
    *sec = (int64_t)(ticks / TIMER_FREQ);
  if (nsec)
    *nsec = (int64_t)(((ticks % TIMER_FREQ) * NSEC_PER_SEC) / TIMER_FREQ);
}

void ticks_to_timeval(uint64_t ticks, int64_t *sec, int64_t *usec) {
  if (sec)
    *sec = (int64_t)(ticks / TIMER_FREQ);
  if (usec)
    *usec = (int64_t)(((ticks % TIMER_FREQ) * USEC_PER_SEC) / TIMER_FREQ);
}

uint64_t ns_to_time_ticks(uint64_t ns) {
  uint64_t sec = ns / NSEC_PER_SEC;
  uint64_t rem = ns % NSEC_PER_SEC;
  uint64_t ticks = saturating_mul(sec, TIMER_FREQ);
  uint64_t rem_ticks = (rem * TIMER_FREQ + NSEC_PER_SEC - 1) / NSEC_PER_SEC;
  return saturating_add(ticks, rem_ticks);
}

uint64_t timespec_to_ticks(uint64_t sec, uint64_t nsec) {
  return saturating_add(saturating_mul(sec, TIMER_FREQ), ns_to_time_ticks(nsec));
}

uint64_t seconds_to_ticks(uint64_t sec) {
  return saturating_mul(sec, TIMER_FREQ);
}

uint64_t timespec_rel_deadline(uint64_t sec, uint64_t nsec) {
  return saturating_add(r_time(), timespec_to_ticks(sec, nsec));
}

int get_wall_time(int64_t *sec, int64_t *nsec) {
  if (!realtime_available_value)
    return -1;

  uint64_t now_ns = saturating_add(boot_epoch_ns,
                                   ticks_to_ns(timekeeping_elapsed_ticks()));
  if (sec)
    *sec = (int64_t)(now_ns / NSEC_PER_SEC);
  if (nsec)
    *nsec = (int64_t)(now_ns % NSEC_PER_SEC);
  return 0;
}

uint64_t timekeeping_now_seconds(void) {
  int64_t sec;
  if (get_wall_time(&sec, NULL) == 0)
    return (uint64_t)sec;
  return timekeeping_elapsed_ticks() / TIMER_FREQ;
}

uint64_t timekeeping_to_clock_ticks(uint64_t ticks) {
  return (ticks * CLK_TCK) / TIMER_FREQ;
}
