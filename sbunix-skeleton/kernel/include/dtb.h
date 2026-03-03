#ifndef _DTB_H
#define _DTB_H

#include <riscv.h>

/* FDT header (all fields big-endian) */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  0x00000001
#define FDT_END_NODE    0x00000002
#define FDT_PROP        0x00000003
#define FDT_NOP         0x00000004
#define FDT_END         0x00000009

/* Byte-swap helpers (FDT is big-endian, RISC-V is little-endian) */
static inline uint32_t fdt32_to_cpu(uint32_t x) {
    return ((x & 0xff) << 24) | ((x & 0xff00) << 8) |
           ((x & 0xff0000) >> 8) | ((x & 0xff000000) >> 24);
}

static inline uint64_t fdt64_to_cpu(uint64_t x) {
    return ((uint64_t)fdt32_to_cpu((uint32_t)x) << 32) |
           fdt32_to_cpu((uint32_t)(x >> 32));
}

/* Device types */
enum dtb_dev_type {
    DTB_DEV_UNKNOWN = 0,
    DTB_DEV_UART,
    DTB_DEV_PLIC,
    DTB_DEV_CLINT,
    DTB_DEV_VIRTIO_MMIO,
    DTB_DEV_PCI,
};

#define DTB_MAX_DEVICES 32

struct dtb_device {
    enum dtb_dev_type type;
    uint64_t base_addr;
    uint64_t size;
    uint32_t irq;
    const char *compatible;
};

struct dtb_info {
    int ndevices;
    struct dtb_device devices[DTB_MAX_DEVICES];
};

extern struct dtb_info dtb_devices;

void dtb_parse(void *dtb);
struct dtb_device *dtb_find_device(enum dtb_dev_type type);
struct dtb_device *dtb_find_device_n(enum dtb_dev_type type, int n);
void dtb_print_devices(void);

#endif
