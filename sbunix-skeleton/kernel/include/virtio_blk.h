#ifndef _VIRTIO_BLK_H
#define _VIRTIO_BLK_H

#include <riscv.h>

/* ---- virtio-mmio register offsets (virtio spec §4.2.2) ---- */
#define VIRTIO_MMIO_MAGIC_VALUE        0x000
#define VIRTIO_MMIO_VERSION            0x004
#define VIRTIO_MMIO_DEVICE_ID          0x008
#define VIRTIO_MMIO_VENDOR_ID          0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES    0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES    0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE    0x028  /* v1 only */
#define VIRTIO_MMIO_QUEUE_SEL          0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX      0x034
#define VIRTIO_MMIO_QUEUE_NUM          0x038
#define VIRTIO_MMIO_QUEUE_ALIGN        0x03c  /* v1 only */
#define VIRTIO_MMIO_QUEUE_PFN          0x040  /* v1 only */
#define VIRTIO_MMIO_QUEUE_READY        0x044  /* v2 only */
#define VIRTIO_MMIO_QUEUE_NOTIFY       0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS   0x060
#define VIRTIO_MMIO_INTERRUPT_ACK      0x064
#define VIRTIO_MMIO_STATUS             0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW     0x080  /* v2 only */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH    0x084  /* v2 only */
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW   0x090  /* v2 only */
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH  0x094  /* v2 only */
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW   0x0a0  /* v2 only */
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH  0x0a4  /* v2 only */
#define VIRTIO_MMIO_CONFIG             0x100

#define VIRTIO_MAGIC 0x74726976  /* "virt" */

/* Device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FAILED       128

/* Feature bits */
#define VIRTIO_F_VERSION_1  (1UL << 32)

/* Block device ID */
#define VIRTIO_DEV_BLK 2

/* Block request types */
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

/* Block request status */
#define VIRTIO_BLK_S_OK    0
#define VIRTIO_BLK_S_IOERR 1

/* Virtqueue size */
#define VIRTQ_SIZE 16

/* Descriptor flags */
#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2

/* ---- Virtqueue structures (virtio spec §2.7) ---- */
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

/* Driver state */
struct virtio_blk_dev {
    volatile uint32_t *base;   /* MMIO base address */
    uint32_t version;          /* virtio-mmio transport version (1 or 2) */
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uint16_t last_used_idx;
    uint64_t capacity;         /* in 512-byte sectors */
};

int virtio_blk_init(void);
int virtio_blk_rw(uint64_t sector, void *buf, int write);
void virtio_blk_test(void);

#endif
