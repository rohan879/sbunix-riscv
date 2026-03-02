#include <virtio_blk.h>
#include <dtb.h>
#include <kalloc.h>
#include <printk.h>
#include <string.h>

static struct virtio_blk_dev blkdev;

static inline uint32_t reg_read(volatile uint32_t *base, uint32_t off) {
    return *(volatile uint32_t *)((char *)base + off);
}

static inline void reg_write(volatile uint32_t *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)((char *)base + off) = val;
}

static inline void mb(void) {
    asm volatile("fence iorw, iorw" ::: "memory");
}

/*
 * Initialize the virtio-blk device.
 * Supports both virtio-mmio v1 (legacy) and v2 (modern) transports.
 */
int virtio_blk_init(void) {
    /* Scan DTB-discovered virtio-mmio slots for a block device */
    volatile uint32_t *base = NULL;
    for (int i = 0; ; i++) {
        struct dtb_device *d = dtb_find_device_n(DTB_DEV_VIRTIO_MMIO, i);
        if (!d) break;
        volatile uint32_t *b = (volatile uint32_t *)d->base_addr;
        if (reg_read(b, VIRTIO_MMIO_MAGIC_VALUE) == VIRTIO_MAGIC &&
            reg_read(b, VIRTIO_MMIO_DEVICE_ID) == VIRTIO_DEV_BLK) {
            base = b;
            printk("virtio_blk: found block device at 0x%lx\n", (uint64_t)b);
            break;
        }
    }

    if (!base) {
        printk("virtio_blk: no block device found\n");
        return -1;
    }

    blkdev.base = base;
    blkdev.version = reg_read(base, VIRTIO_MMIO_VERSION);
    printk("virtio_blk: transport version %d\n", blkdev.version);

    /* §4.2.3.1 Device Initialization */
    /* 1. Reset */
    reg_write(base, VIRTIO_MMIO_STATUS, 0);
    mb();

    /* 2. Set ACKNOWLEDGE */
    uint32_t status = 0;
    status |= VIRTIO_STATUS_ACKNOWLEDGE;
    reg_write(base, VIRTIO_MMIO_STATUS, status);

    /* 3. Set DRIVER */
    status |= VIRTIO_STATUS_DRIVER;
    reg_write(base, VIRTIO_MMIO_STATUS, status);

    /* 4. Negotiate features */
    if (blkdev.version == 2) {
        /* v2: use feature select registers */
        reg_write(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
        mb();
        uint32_t dev_feat_hi = reg_read(base, VIRTIO_MMIO_DEVICE_FEATURES);
        uint32_t drv_feat_hi = dev_feat_hi & (1U << 0); /* VIRTIO_F_VERSION_1 */
        reg_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
        reg_write(base, VIRTIO_MMIO_DRIVER_FEATURES, drv_feat_hi);
        reg_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
        reg_write(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);

        /* 5. Set FEATURES_OK */
        status |= VIRTIO_STATUS_FEATURES_OK;
        reg_write(base, VIRTIO_MMIO_STATUS, status);
        mb();

        /* 6. Verify FEATURES_OK */
        if (!(reg_read(base, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
            printk("virtio_blk: FEATURES_OK not set by device\n");
            reg_write(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
            return -1;
        }
    } else {
        /* v1: no feature select; read/write features directly, no FEATURES_OK step */
        uint32_t dev_feat = reg_read(base, VIRTIO_MMIO_DEVICE_FEATURES);
        (void)dev_feat;
        reg_write(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    }

    /* 7. Set up virtqueue 0 */
    reg_write(base, VIRTIO_MMIO_QUEUE_SEL, 0);
    mb();

    uint32_t qmax = reg_read(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0) {
        printk("virtio_blk: queue not available\n");
        return -1;
    }
    uint32_t qsz = VIRTQ_SIZE;
    if (qsz > qmax) qsz = qmax;

    reg_write(base, VIRTIO_MMIO_QUEUE_NUM, qsz);

    if (blkdev.version == 2) {
        /* v2: allocate separate pages, set individual addresses */
        char *page0 = kalloc();
        char *page1 = kalloc();
        char *page2 = kalloc();
        if (!page0 || !page1 || !page2) {
            printk("virtio_blk: out of memory\n");
            return -1;
        }
        memset(page0, 0, PAGE_SIZE);
        memset(page1, 0, PAGE_SIZE);
        memset(page2, 0, PAGE_SIZE);

        blkdev.desc  = (struct virtq_desc *)page0;
        blkdev.avail = (struct virtq_avail *)page1;
        blkdev.used  = (struct virtq_used *)page2;

        uint64_t desc_pa  = (uint64_t)page0;
        uint64_t avail_pa = (uint64_t)page1;
        uint64_t used_pa  = (uint64_t)page2;

        reg_write(base, VIRTIO_MMIO_QUEUE_DESC_LOW,    (uint32_t)desc_pa);
        reg_write(base, VIRTIO_MMIO_QUEUE_DESC_HIGH,   (uint32_t)(desc_pa >> 32));
        reg_write(base, VIRTIO_MMIO_QUEUE_DRIVER_LOW,  (uint32_t)avail_pa);
        reg_write(base, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint32_t)(avail_pa >> 32));
        reg_write(base, VIRTIO_MMIO_QUEUE_DEVICE_LOW,  (uint32_t)used_pa);
        reg_write(base, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uint32_t)(used_pa >> 32));

        reg_write(base, VIRTIO_MMIO_QUEUE_READY, 1);
        mb();
    } else {
        /* v1: contiguous layout, set GUEST_PAGE_SIZE and QUEUE_PFN.
         *
         * The device computes ring offsets from a single base address:
         *   desc  at  base
         *   avail at  base + 16 * qsz
         *   used  at  align(base + 16*qsz + 6 + 2*qsz, QUEUE_ALIGN)
         *
         * For qsz=16 and QUEUE_ALIGN=4096, used starts at base + 4096.
         * We need 2 CONTIGUOUS pages.  kalloc() returns pages from a
         * high-to-low free list, so the second call returns the page
         * just below the first.  We use the lower one as base.
         */
        reg_write(base, VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);

        char *p1 = kalloc();
        char *p2 = kalloc();
        if (!p1 || !p2) {
            printk("virtio_blk: out of memory\n");
            return -1;
        }

        /* Sort: lo is the lower address, hi = lo + PAGE_SIZE */
        char *lo, *hi;
        if ((uint64_t)p1 < (uint64_t)p2) { lo = p1; hi = p2; }
        else                              { lo = p2; hi = p1; }

        if (hi != lo + PAGE_SIZE) {
            /* Pages not contiguous — try one more */
            char *p3 = kalloc();
            if (!p3) { printk("virtio_blk: out of memory\n"); return -1; }
            if (p3 + PAGE_SIZE == lo)      { kfree(hi); lo = p3; }
            else if (hi + PAGE_SIZE == p3) { kfree(lo); lo = hi; hi = p3; }
            else if (p3 == lo + PAGE_SIZE) { kfree(hi); hi = p3; } /* lo,p3 */
            else if (lo == p3 + PAGE_SIZE) { kfree(hi); hi = lo; lo = p3; }
            else { printk("virtio_blk: cannot get contiguous pages\n"); return -1; }
        }

        memset(lo, 0, 2 * PAGE_SIZE);

        blkdev.desc  = (struct virtq_desc *)lo;
        blkdev.avail = (struct virtq_avail *)(lo + qsz * sizeof(struct virtq_desc));
        blkdev.used  = (struct virtq_used *)hi;

        reg_write(base, VIRTIO_MMIO_QUEUE_ALIGN, PAGE_SIZE);
        reg_write(base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)((uint64_t)lo / PAGE_SIZE));
        mb();
    }

    blkdev.last_used_idx = 0;

    /* 8. Read block device capacity from config space */
    uint32_t cap_lo = reg_read(base, VIRTIO_MMIO_CONFIG + 0);
    uint32_t cap_hi = reg_read(base, VIRTIO_MMIO_CONFIG + 4);
    blkdev.capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    /* 9. Set DRIVER_OK */
    status |= VIRTIO_STATUS_DRIVER_OK;
    reg_write(base, VIRTIO_MMIO_STATUS, status);
    mb();

    printk("virtio_blk: initialized, capacity=%ld sectors (%ld KB)\n",
           (long)blkdev.capacity, (long)(blkdev.capacity / 2));

    return 0;
}

int virtio_blk_rw(uint64_t sector, void *buf, int write) {
    /* Build 3-descriptor chain: header, data, status */
    static struct virtio_blk_req req;
    static uint8_t status_byte;

    req.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    req.reserved = 0;
    req.sector = sector;
    status_byte = 0xff;

    /* Descriptor 0: header (device-readable) */
    blkdev.desc[0].addr  = (uint64_t)&req;
    blkdev.desc[0].len   = sizeof(req);
    blkdev.desc[0].flags = VIRTQ_DESC_F_NEXT;
    blkdev.desc[0].next  = 1;

    /* Descriptor 1: data buffer */
    blkdev.desc[1].addr  = (uint64_t)buf;
    blkdev.desc[1].len   = 4096;
    blkdev.desc[1].flags = VIRTQ_DESC_F_NEXT;
    if (!write)
        blkdev.desc[1].flags |= VIRTQ_DESC_F_WRITE; /* device writes to buf */
    blkdev.desc[1].next  = 2;

    /* Descriptor 2: status byte (device-writable) */
    blkdev.desc[2].addr  = (uint64_t)&status_byte;
    blkdev.desc[2].len   = 1;
    blkdev.desc[2].flags = VIRTQ_DESC_F_WRITE;
    blkdev.desc[2].next  = 0;

    /* Add to available ring */
    uint16_t avail_idx = blkdev.avail->idx;
    blkdev.avail->ring[avail_idx % VIRTQ_SIZE] = 0; /* head descriptor index */
    mb();
    blkdev.avail->idx = avail_idx + 1;
    mb();

    /* Notify device */
    reg_write(blkdev.base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* Poll for completion */
    while (blkdev.used->idx == blkdev.last_used_idx) {
        mb();
    }
    blkdev.last_used_idx++;

    /* Acknowledge interrupt */
    uint32_t isr = reg_read(blkdev.base, VIRTIO_MMIO_INTERRUPT_STATUS);
    reg_write(blkdev.base, VIRTIO_MMIO_INTERRUPT_ACK, isr);

    if (status_byte != VIRTIO_BLK_S_OK) {
        printk("virtio_blk: I/O error, status=%d\n", status_byte);
        return -1;
    }

    return 0;
}

void virtio_blk_test(void) {
    if (virtio_blk_init() < 0) {
        printk("virtio_blk_test: FAILED (init error)\n");
        return;
    }

    /* Allocate a 4KB buffer page for I/O */
    char *buf = kalloc();
    if (!buf) {
        printk("virtio_blk_test: FAILED (out of memory)\n");
        return;
    }

    int nblocks = 100;

    /* Write phase: block N is filled with byte value (N & 0xff) */
    printk("virtio_blk_test: writing %d blocks...\n", nblocks);
    for (int i = 0; i < nblocks; i++) {
        memset(buf, i & 0xff, 4096);
        /* Each 4KB block = 8 sectors */
        if (virtio_blk_rw((uint64_t)i * 8, buf, 1) < 0) {
            printk("virtio_blk_test: FAILED (write error at block %d)\n", i);
            kfree(buf);
            return;
        }
    }

    /* Read + verify phase */
    printk("virtio_blk_test: reading back and verifying...\n");
    int pass = 1;
    for (int i = 0; i < nblocks; i++) {
        memset(buf, 0xaa, 4096);
        if (virtio_blk_rw((uint64_t)i * 8, buf, 0) < 0) {
            printk("virtio_blk_test: FAILED (read error at block %d)\n", i);
            kfree(buf);
            return;
        }
        unsigned char expected = i & 0xff;
        for (int j = 0; j < 4096; j++) {
            if ((unsigned char)buf[j] != expected) {
                printk("virtio_blk_test: FAILED block %d byte %d: expected 0x%x got 0x%x\n",
                       i, j, expected, (unsigned char)buf[j]);
                pass = 0;
                break;
            }
        }
        if (!pass) break;
    }

    if (pass) {
        printk("virtio_blk_test: ALL %d BLOCKS PASSED\n", nblocks);
    }

    kfree(buf);
}
