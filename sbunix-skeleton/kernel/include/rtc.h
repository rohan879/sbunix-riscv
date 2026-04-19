#ifndef _RTC_H
#define _RTC_H

#include <riscv.h>

void rtc_init(void);
int rtc_available(void);
uint64_t rtc_read_epoch_ns(void);

#endif
