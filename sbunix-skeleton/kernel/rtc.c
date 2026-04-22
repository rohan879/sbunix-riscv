#include <dtb.h>
#include <rtc.h>

#define GOLDFISH_RTC_FALLBACK_BASE 0x101000UL
#define GOLDFISH_RTC_FALLBACK_SIZE 0x1000UL
#define GOLDFISH_RTC_TIME_LOW  0x00
#define GOLDFISH_RTC_TIME_HIGH 0x04

static uint64_t rtc_base;
static uint64_t rtc_size;

void rtc_init(void) {
  struct dtb_device *rtc = dtb_find_device(DTB_DEV_RTC);
  if (rtc) {
    rtc_base = rtc->base_addr;
    rtc_size = rtc->size;
  } else {
    rtc_base = GOLDFISH_RTC_FALLBACK_BASE;
    rtc_size = GOLDFISH_RTC_FALLBACK_SIZE;
  }
}

int rtc_available(void) {
  return rtc_base != 0 && rtc_size >= 8;
}

uint64_t rtc_read_epoch_ns(void) {
  volatile uint32_t *lo_reg =
      (volatile uint32_t *)(rtc_base + GOLDFISH_RTC_TIME_LOW);
  volatile uint32_t *hi_reg =
      (volatile uint32_t *)(rtc_base + GOLDFISH_RTC_TIME_HIGH);
  uint32_t hi1;
  uint32_t hi2;
  uint32_t lo;

  do {
    hi1 = *hi_reg;
    lo = *lo_reg;
    hi2 = *hi_reg;
  } while (hi1 != hi2);

  return ((uint64_t)hi1 << 32) | (uint64_t)lo;
}
