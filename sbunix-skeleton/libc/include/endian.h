#ifndef _ENDIAN_H
#define _ENDIAN_H

#include <stdint.h>

#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN    4321
#define BYTE_ORDER    LITTLE_ENDIAN

static inline uint16_t __bswap16(uint16_t x)
{
  return (uint16_t)((x << 8) | (x >> 8));
}

static inline uint32_t __bswap32(uint32_t x)
{
  return ((x & 0x000000ffU) << 24) |
         ((x & 0x0000ff00U) << 8) |
         ((x & 0x00ff0000U) >> 8) |
         ((x & 0xff000000U) >> 24);
}

static inline uint64_t __bswap64(uint64_t x)
{
  return ((uint64_t)__bswap32((uint32_t)x) << 32) |
         (uint64_t)__bswap32((uint32_t)(x >> 32));
}

#define htobe16(x) __bswap16((uint16_t)(x))
#define htole16(x) ((uint16_t)(x))
#define be16toh(x) __bswap16((uint16_t)(x))
#define le16toh(x) ((uint16_t)(x))

#define htobe32(x) __bswap32((uint32_t)(x))
#define htole32(x) ((uint32_t)(x))
#define be32toh(x) __bswap32((uint32_t)(x))
#define le32toh(x) ((uint32_t)(x))

#define htobe64(x) __bswap64((uint64_t)(x))
#define htole64(x) ((uint64_t)(x))
#define be64toh(x) __bswap64((uint64_t)(x))
#define le64toh(x) ((uint64_t)(x))

#endif
