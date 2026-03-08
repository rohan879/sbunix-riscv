#ifndef _VIRTIO_BLK_H
#define _VIRTIO_BLK_H

#include <riscv.h>

/* ------------------------------------------------------------------ *
 * PCI configuration space (standard type-0 header)                    *
 * All offsets relative to the ECAM slot base (bus<<20|dev<<15|fn<<12) *
 * ------------------------------------------------------------------ */
#define PCI_CFG_VENDOR_ID       0x00  /* 16-bit */
#define PCI_CFG_DEVICE_ID       0x02  /* 16-bit */
#define PCI_CFG_COMMAND         0x04  /* 16-bit */
#define PCI_CFG_STATUS          0x06  /* 16-bit */
#define PCI_CFG_REVISION        0x08
#define PCI_CFG_PROGIF          0x09
#define PCI_CFG_SUBCLASS        0x0a
#define PCI_CFG_CLASS           0x0b
#define PCI_CFG_HEADER_TYPE     0x0e
#define PCI_CFG_BAR0            0x10  /* 32-bit */
#define PCI_CFG_BAR1            0x14  /* 32-bit (BAR0 high word for 64-bit BAR) */
#define PCI_CFG_BAR2            0x18
#define PCI_CFG_BAR3            0x1c
#define PCI_CFG_BAR4            0x20
#define PCI_CFG_BAR5            0x24
#define PCI_CFG_CAP_PTR         0x34  /* 8-bit: offset of first capability */

/* PCI command register bits */
#define PCI_CMD_IO_SPACE        (1 << 0)
#define PCI_CMD_MEM_SPACE       (1 << 1)
#define PCI_CMD_BUS_MASTER      (1 << 2)

/* PCI status register bits */
#define PCI_STATUS_CAP_LIST     (1 << 4)

/* PCI BAR type bits */
#define PCI_BAR_IO              (1 << 0)   /* set => I/O space (not MMIO) */
#define PCI_BAR_TYPE_MASK       (3 << 1)
#define PCI_BAR_TYPE_64BIT      (2 << 1)
#define PCI_BAR_ADDR_MASK32     0xfffffff0u

/* PCI capability list: first byte of each cap is the cap ID */
#define PCI_CAP_ID_VNDR         0x09  /* vendor-specific capability */

/* ------------------------------------------------------------------ *
 * virtio-over-PCI (spec §4.1)                                         *
 * ------------------------------------------------------------------ */

/* Virtio vendor / device IDs */
#define VIRTIO_PCI_VENDOR_ID    0x1af4
#define VIRTIO_PCI_DEV_BLK_NONTRANS  0x1042  /* non-transitional block device */

/* Virtio capability types (virtio_pci_cap.cfg_type) */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5

/*
 * virtio_pci_cap — layout of a virtio vendor-specific PCI capability.
 * Lives in PCI config space starting at the cap's config-space offset.
 */
struct virtio_pci_cap {
    uint8_t  cap_vndr;    /* 0x09 = PCI_CAP_ID_VNDR */
    uint8_t  cap_next;    /* next cap offset (0 = end) */
    uint8_t  cap_len;     /* length of this cap structure */
    uint8_t  cfg_type;    /* VIRTIO_PCI_CAP_* */
    uint8_t  bar;         /* which BAR holds the register region */
    uint8_t  id;          /* multiple caps of same type: distinguish them */
    uint8_t  padding[2];
    uint32_t offset;      /* byte offset within that BAR */
    uint32_t length;      /* byte length of the region */
} __attribute__((packed));

/* NOTIFY cap appends one extra 32-bit field */
struct virtio_pci_notify_cap {
    struct virtio_pci_cap cap;
    uint32_t notify_off_multiplier;
} __attribute__((packed));

/* ------------------------------------------------------------------ *
 * virtio_pci_common_cfg — register offsets within the common-cfg BAR  *
 * region (virtio spec §4.1.4.3, table 4.2).                           *
 *                                                                      *
 * These are used with the ccfg_read/ccfg_write helpers in the        *
 * driver instead of a packed struct to avoid compiler-generated        *
 * unaligned access issues.                                             *
 * ------------------------------------------------------------------ */
#define VCFG_DEVICE_FEATURE_SEL  0x00  /* u32 rw */
#define VCFG_DEVICE_FEATURE      0x04  /* u32 ro */
#define VCFG_DRIVER_FEATURE_SEL  0x08  /* u32 rw */
#define VCFG_DRIVER_FEATURE      0x0c  /* u32 rw */
#define VCFG_MSIX_CONFIG         0x10  /* u16 rw */
#define VCFG_NUM_QUEUES          0x12  /* u16 ro */
#define VCFG_DEVICE_STATUS       0x14  /* u8  rw */
#define VCFG_CONFIG_GENERATION   0x15  /* u8  ro */
#define VCFG_QUEUE_SELECT        0x16  /* u16 rw */
#define VCFG_QUEUE_SIZE          0x18  /* u16 rw */
#define VCFG_QUEUE_MSIX_VECTOR   0x1a  /* u16 rw */
#define VCFG_QUEUE_ENABLE        0x1c  /* u16 rw */
#define VCFG_QUEUE_NOTIFY_OFF    0x1e  /* u16 ro */
#define VCFG_QUEUE_DESC_LO       0x20  /* u32 rw */
#define VCFG_QUEUE_DESC_HI       0x24  /* u32 rw */
#define VCFG_QUEUE_DRIVER_LO     0x28  /* u32 rw */
#define VCFG_QUEUE_DRIVER_HI     0x2c  /* u32 rw */
#define VCFG_QUEUE_DEVICE_LO     0x30  /* u32 rw */
#define VCFG_QUEUE_DEVICE_HI     0x34  /* u32 rw */

/* Device status bits (virtio spec §2.1) */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

/* Feature bits */
#define VIRTIO_F_VERSION_1_BIT  0   /* bit 32 in the 64-bit set => word 1, bit 0 */

/* Block device ID (virtio spec §5.2) */
#define VIRTIO_DEV_BLK 2

/* Block request types */
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

/* Block request status */
#define VIRTIO_BLK_S_OK    0
#define VIRTIO_BLK_S_IOERR 1

/* Virtqueue size used by this driver */
#define VIRTQ_SIZE 16

/* Descriptor flags */
#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2

/* ------------------------------------------------------------------ *
 * Virtqueue structures (virtio spec §2.7)                             *
 * ------------------------------------------------------------------ */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_SIZE];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTQ_SIZE];
};

/* Block request header */
struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/* ------------------------------------------------------------------ *
 * Driver state                                                         *
 * ------------------------------------------------------------------ */
struct virtio_blk_dev {
    /* virtio common config region base (COMMON_CFG BAR + cap offset) */
    volatile uint8_t  *common;

    /* notify region base + per-queue notify offset multiplier */
    volatile uint8_t  *notify_base;
    uint32_t           notify_off_multiplier;
    uint16_t           queue_notify_off; /* queue 0's notify_off */

    /* device-specific config region (capacity at offset 0) */
    volatile uint32_t *devcfg;

    /* virtqueue rings */
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint16_t            last_used_idx;

    uint64_t capacity;   /* in 512-byte sectors */
};

int  virtio_blk_init(void);
int  virtio_blk_rw(uint64_t sector, void *buf, int write);
void virtio_blk_test(void);
uint64_t virtio_blk_capacity(void);

#endif
