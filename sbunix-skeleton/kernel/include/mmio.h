#ifndef _MMIO_H
#define _MMIO_H

#include <riscv.h>

static inline uint8_t mmio_read8(const volatile void *addr) {
  return *(const volatile uint8_t *)addr;
}

static inline uint16_t mmio_read16(const volatile void *addr) {
  return *(const volatile uint16_t *)addr;
}

static inline uint32_t mmio_read32(const volatile void *addr) {
  return *(const volatile uint32_t *)addr;
}

static inline void mmio_write8(volatile void *addr, uint8_t val) {
  *(volatile uint8_t *)addr = val;
}

static inline void mmio_write16(volatile void *addr, uint16_t val) {
  *(volatile uint16_t *)addr = val;
}

static inline void mmio_write32(volatile void *addr, uint32_t val) {
  *(volatile uint32_t *)addr = val;
}

static inline void mmio_fence_iorw(void) {
  asm volatile("fence iorw, iorw" ::: "memory");
}

#endif
