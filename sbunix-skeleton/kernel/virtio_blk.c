#include <virtio_blk.h>
#include <kalloc.h>
#include <printk.h>
#include <string.h>

static struct virtio_blk_dev blkdev;

/* ------------------------------------------------------------------ *
 * PCI ECAM helpers                                                     *
 *                                                                      *
 * QEMU virt machine: ECAM base = 0x30000000                           *
 * Config space address = base | (bus<<20) | (dev<<15) | (fn<<12)     *
 * ------------------------------------------------------------------ */
#define PCI_ECAM_BASE  0x30000000UL

static inline volatile void *ecam_addr(uint32_t bus, uint32_t dev,
                                        uint32_t fn,  uint32_t off) {
    return (volatile void *)(PCI_ECAM_BASE |
                             (bus << 20) | (dev << 15) | (fn << 12) | off);
}

static inline uint8_t pci_read8(uint32_t bus, uint32_t dev,
                                 uint32_t fn,  uint32_t off) {
    return *(volatile uint8_t *)ecam_addr(bus, dev, fn, off);
}

static inline uint16_t pci_read16(uint32_t bus, uint32_t dev,
                                   uint32_t fn,  uint32_t off) {
    return *(volatile uint16_t *)ecam_addr(bus, dev, fn, off);
}

static inline uint32_t pci_read32(uint32_t bus, uint32_t dev,
                                   uint32_t fn,  uint32_t off) {
    return *(volatile uint32_t *)ecam_addr(bus, dev, fn, off);
}

static inline void pci_write16(uint32_t bus, uint32_t dev,
                                uint32_t fn,  uint32_t off, uint16_t val) {
    *(volatile uint16_t *)ecam_addr(bus, dev, fn, off) = val;
}

static inline void pci_write32(uint32_t bus, uint32_t dev,
                                uint32_t fn,  uint32_t off, uint32_t val) {
    *(volatile uint32_t *)ecam_addr(bus, dev, fn, off) = val;
}

/* Memory barrier */
static inline void mb(void) {
    asm volatile("fence iorw, iorw" ::: "memory");
}

/* ------------------------------------------------------------------ *
 * Common-config region accessors (use explicit byte offsets to avoid  *
 * packed-struct unaligned-access code-gen issues on RISC-V).          *
 * ------------------------------------------------------------------ */
static inline uint8_t  ccfg_r8 (volatile uint8_t *b, uint32_t off) {
    return *(volatile uint8_t  *)(b + off);
}
static inline uint16_t ccfg_r16(volatile uint8_t *b, uint32_t off) {
    return *(volatile uint16_t *)(b + off);
}
static inline uint32_t ccfg_r32(volatile uint8_t *b, uint32_t off) {
    return *(volatile uint32_t *)(b + off);
}
static inline void ccfg_w8 (volatile uint8_t *b, uint32_t off, uint8_t  v) {
    *(volatile uint8_t  *)(b + off) = v;
}
static inline void ccfg_w16(volatile uint8_t *b, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(b + off) = v;
}
static inline void ccfg_w32(volatile uint8_t *b, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(b + off) = v;
}

/* ------------------------------------------------------------------ *
 * Simple bump allocator for PCI MMIO BAR space.                       *
 *                                                                      *
 * QEMU virt: PCI MMIO window = 0x40000000 – 0x80000000 (1 GiB).      *
 * OpenSBI does NOT enumerate PCI BARs, so we must do it ourselves.    *
 * ------------------------------------------------------------------ */
#define PCI_MMIO_BASE  0x40000000UL
#define PCI_MMIO_END   0x80000000UL

static uint64_t pci_mmio_alloc_ptr = PCI_MMIO_BASE;

static uint64_t pci_alloc_mmio(uint64_t size) {
    /* Align up to the requested size (power-of-two) */
    pci_mmio_alloc_ptr = (pci_mmio_alloc_ptr + size - 1) & ~(size - 1);
    uint64_t base = pci_mmio_alloc_ptr;
    pci_mmio_alloc_ptr += size;
    return base;
}

/* ------------------------------------------------------------------ *
 * BAR programmer: probes BAR size, allocates MMIO space, programs the *
 * BAR.  Returns the physical base address of the BAR.                  *
 *                                                                      *
 * Works for 32-bit and 64-bit memory BARs.  I/O BARs are skipped.    *
 * ------------------------------------------------------------------ */
static uint64_t pci_bar_alloc(uint32_t bus, uint32_t dev, uint32_t fn,
                               uint32_t bar_idx) {
    uint32_t bar_off = PCI_CFG_BAR0 + bar_idx * 4;
    uint32_t orig    = pci_read32(bus, dev, fn, bar_off);

    /* I/O space BARs: skip */
    if (orig & PCI_BAR_IO)
        return 0;

    int is64 = ((orig & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64BIT);

    /* Probe size: write all-ones, read back, restore */
    pci_write32(bus, dev, fn, bar_off, 0xffffffffu);
    uint32_t sz_lo = pci_read32(bus, dev, fn, bar_off);
    pci_write32(bus, dev, fn, bar_off, orig);

    uint64_t size;
    if (is64) {
        uint32_t orig_hi = pci_read32(bus, dev, fn, bar_off + 4);
        pci_write32(bus, dev, fn, bar_off + 4, 0xffffffffu);
        uint32_t sz_hi = pci_read32(bus, dev, fn, bar_off + 4);
        pci_write32(bus, dev, fn, bar_off + 4, orig_hi);
        uint64_t sz_raw = ((uint64_t)sz_hi << 32) | (sz_lo & PCI_BAR_ADDR_MASK32);
        size = (~sz_raw) + 1; /* two's complement size */
    } else {
        uint32_t sz_raw = sz_lo & PCI_BAR_ADDR_MASK32;
        size = (uint64_t)((~sz_raw) + 1) & 0xffffffff;
    }

    if (size == 0)
        return 0;

    /* If already assigned (non-zero address), use existing value */
    uint64_t existing;
    if (is64) {
        uint32_t hi = pci_read32(bus, dev, fn, bar_off + 4);
        existing = ((uint64_t)hi << 32) | (orig & PCI_BAR_ADDR_MASK32);
    } else {
        existing = orig & PCI_BAR_ADDR_MASK32;
    }
    if (existing != 0)
        return existing;

    /* Allocate from our MMIO pool and program the BAR */
    uint64_t base = pci_alloc_mmio(size);
    pci_write32(bus, dev, fn, bar_off, (uint32_t)base | (orig & ~PCI_BAR_ADDR_MASK32));
    if (is64)
        pci_write32(bus, dev, fn, bar_off + 4, (uint32_t)(base >> 32));

    printk("virtio_blk: BAR%d assigned 0x%lx size=0x%lx\n",
           bar_idx, base, size);
    return base;
}

/* ------------------------------------------------------------------ *
 * virtio_blk_init                                                      *
 *                                                                      *
 * 1. Scan bus 0 for PCI device vendor=0x1af4 device=0x1042            *
 * 2. Allocate/read BARs (OpenSBI doesn't do PCI enumeration)          *
 * 3. Enable memory decode + bus-master                                 *
 * 4. Walk capability list for virtio PCI caps (type 9)                *
 *    - COMMON_CFG  -> base pointer for ccfg_r/ccfg_w helpers           *
 *    - NOTIFY_CFG  -> notify BAR base + multiplier                    *
 *    - DEVICE_CFG  -> device-specific config (capacity)               *
 * 5. Run virtio-modern (version 1) init sequence                       *
 * 6. Set up virtqueue 0                                                *
 * ------------------------------------------------------------------ */
int virtio_blk_init(void) {
    /* --- 1. Scan PCI bus 0 --- */
    uint32_t found_bus = 0, found_dev = 0, found_fn = 0;
    int found = 0;

    for (uint32_t bus = 0; bus < 256 && !found; bus++) {
        for (uint32_t dev = 0; dev < 32 && !found; dev++) {
            /* Check whether slot is populated */
            uint16_t vid = pci_read16(bus, dev, 0, PCI_CFG_VENDOR_ID);
            if (vid == 0xffff)
                continue;

            uint8_t hdr_type = pci_read8(bus, dev, 0, PCI_CFG_HEADER_TYPE);
            uint32_t nfn = (hdr_type & 0x80) ? 8 : 1; /* multi-function? */

            for (uint32_t fn = 0; fn < nfn && !found; fn++) {
                vid = pci_read16(bus, dev, fn, PCI_CFG_VENDOR_ID);
                if (vid != VIRTIO_PCI_VENDOR_ID)
                    continue;
                uint16_t did = pci_read16(bus, dev, fn, PCI_CFG_DEVICE_ID);
                if (did != VIRTIO_PCI_DEV_BLK_NONTRANS)
                    continue;
                found_bus = bus;
                found_dev = dev;
                found_fn  = fn;
                found = 1;
            }
        }
    }

    if (!found) {
        printk("virtio_blk: no block device found\n");
        return -1;
    }

    printk("virtio_blk: found device at PCI %d:%d.%d\n",
           found_bus, found_dev, found_fn);

    uint32_t bus = found_bus, dev = found_dev, fn = found_fn;

    /* --- 2. Allocate / read BARs --- */
    /* virtio-blk-pci-non-transitional uses up to 6 BARs (indices 0-5). */
    uint64_t bar_addr[6] = {0};
    for (uint32_t i = 0; i < 6; ) {
        uint32_t bar_off = PCI_CFG_BAR0 + i * 4;
        uint32_t bar_val = pci_read32(bus, dev, fn, bar_off);
        if (bar_val == 0) { i++; continue; } /* unimplemented BAR */
        bar_addr[i] = pci_bar_alloc(bus, dev, fn, i);
        /* 64-bit BAR occupies two slots */
        if (!(bar_val & PCI_BAR_IO) &&
            (bar_val & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64BIT) {
            i += 2;
        } else {
            i++;
        }
    }

    /* --- 3. Enable memory decode and bus-master --- */
    uint16_t cmd = pci_read16(bus, dev, fn, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_write16(bus, dev, fn, PCI_CFG_COMMAND, cmd);
    mb();

    /* --- 4. Walk capability list for virtio caps --- */
    uint16_t status = pci_read16(bus, dev, fn, PCI_CFG_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) {
        printk("virtio_blk: device has no capability list\n");
        return -1;
    }

    uint8_t cap_ptr = pci_read8(bus, dev, fn, PCI_CFG_CAP_PTR) & 0xfc;

    volatile uint8_t *common   = NULL;
    volatile uint8_t *notify_b = NULL;
    uint32_t notify_off_mul    = 0;
    volatile uint32_t *devcfg  = NULL;

    while (cap_ptr != 0) {
        uint8_t cap_id   = pci_read8(bus, dev, fn, cap_ptr);
        uint8_t cap_next = pci_read8(bus, dev, fn, cap_ptr + 1);

        if (cap_id == PCI_CAP_ID_VNDR) {
            uint8_t cfg_type = pci_read8(bus, dev, fn, cap_ptr + 3);
            uint8_t bar_idx  = pci_read8(bus, dev, fn, cap_ptr + 4);
            uint32_t offset  = pci_read32(bus, dev, fn, cap_ptr + 8);

            volatile uint8_t *region =
                (volatile uint8_t *)bar_addr[bar_idx] + offset;

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                common = region;
                printk("virtio_blk: COMMON_CFG at 0x%lx\n", (uint64_t)region);
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                notify_b = region;
                notify_off_mul = pci_read32(bus, dev, fn, cap_ptr + 16);
                printk("virtio_blk: NOTIFY_CFG at 0x%lx mul=%d\n",
                       (uint64_t)region, notify_off_mul);
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                devcfg = (volatile uint32_t *)region;
                printk("virtio_blk: DEVICE_CFG at 0x%lx\n", (uint64_t)region);
                break;
            default:
                break;
            }
        }

        cap_ptr = cap_next & 0xfc;
    }

    if (!common) {
        printk("virtio_blk: no COMMON_CFG capability\n");
        return -1;
    }
    if (!notify_b) {
        printk("virtio_blk: no NOTIFY_CFG capability\n");
        return -1;
    }
    if (!devcfg) {
        printk("virtio_blk: no DEVICE_CFG capability\n");
        return -1;
    }

    /* --- 5. virtio-modern init sequence (spec §3.1) --- */

    /* 5a. Reset device */
    ccfg_w8(common, VCFG_DEVICE_STATUS, 0);
    mb();
    while (ccfg_r8(common, VCFG_DEVICE_STATUS) != 0)
        mb();

    /* 5b. ACKNOWLEDGE */
    ccfg_w8(common, VCFG_DEVICE_STATUS,
            ccfg_r8(common, VCFG_DEVICE_STATUS) | VIRTIO_STATUS_ACKNOWLEDGE);
    mb();

    /* 5c. DRIVER */
    ccfg_w8(common, VCFG_DEVICE_STATUS,
            ccfg_r8(common, VCFG_DEVICE_STATUS) | VIRTIO_STATUS_DRIVER);
    mb();

    /* 5d. Negotiate features: accept VIRTIO_F_VERSION_1 (word 1 bit 0) */
    ccfg_w32(common, VCFG_DEVICE_FEATURE_SEL, 1);
    mb();
    uint32_t feat_hi = ccfg_r32(common, VCFG_DEVICE_FEATURE);
    ccfg_w32(common, VCFG_DRIVER_FEATURE_SEL, 1);
    mb();
    ccfg_w32(common, VCFG_DRIVER_FEATURE,
             feat_hi & (1U << VIRTIO_F_VERSION_1_BIT));
    mb();
    ccfg_w32(common, VCFG_DRIVER_FEATURE_SEL, 0);
    mb();
    ccfg_w32(common, VCFG_DRIVER_FEATURE, 0);
    mb();

    /* 5e. FEATURES_OK */
    ccfg_w8(common, VCFG_DEVICE_STATUS,
            ccfg_r8(common, VCFG_DEVICE_STATUS) | VIRTIO_STATUS_FEATURES_OK);
    mb();

    /* 5f. Verify FEATURES_OK */
    if (!(ccfg_r8(common, VCFG_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        printk("virtio_blk: FEATURES_OK rejected\n");
        ccfg_w8(common, VCFG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* --- 6. Set up virtqueue 0 --- */
    ccfg_w16(common, VCFG_QUEUE_SELECT, 0);
    mb();

    uint16_t qmax = ccfg_r16(common, VCFG_QUEUE_SIZE);
    if (qmax == 0) {
        printk("virtio_blk: queue 0 not available\n");
        return -1;
    }
    uint16_t qsz = VIRTQ_SIZE;
    if (qsz > qmax) qsz = qmax;
    ccfg_w16(common, VCFG_QUEUE_SIZE, qsz);

    /* Allocate separate pages for descriptor table, driver ring, device ring */
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

    ccfg_w32(common, VCFG_QUEUE_DESC_LO,   (uint32_t)desc_pa);
    ccfg_w32(common, VCFG_QUEUE_DESC_HI,   (uint32_t)(desc_pa >> 32));
    ccfg_w32(common, VCFG_QUEUE_DRIVER_LO, (uint32_t)avail_pa);
    ccfg_w32(common, VCFG_QUEUE_DRIVER_HI, (uint32_t)(avail_pa >> 32));
    ccfg_w32(common, VCFG_QUEUE_DEVICE_LO, (uint32_t)used_pa);
    ccfg_w32(common, VCFG_QUEUE_DEVICE_HI, (uint32_t)(used_pa >> 32));
    mb();

    /* Read per-queue notify offset before enabling the queue */
    uint16_t q_notify_off = ccfg_r16(common, VCFG_QUEUE_NOTIFY_OFF);

    ccfg_w16(common, VCFG_QUEUE_ENABLE, 1);
    mb();

    /* --- Save driver state --- */
    blkdev.common              = common;
    blkdev.notify_base         = notify_b;
    blkdev.notify_off_multiplier = notify_off_mul;
    blkdev.queue_notify_off    = q_notify_off;
    blkdev.devcfg              = devcfg;
    blkdev.last_used_idx       = 0;

    /* --- Read capacity from device-specific config --- */
    uint32_t cap_lo = devcfg[0];
    uint32_t cap_hi = devcfg[1];
    blkdev.capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    /* --- DRIVER_OK --- */
    ccfg_w8(common, VCFG_DEVICE_STATUS,
            ccfg_r8(common, VCFG_DEVICE_STATUS) | VIRTIO_STATUS_DRIVER_OK);
    mb();

    printk("virtio_blk: initialized, capacity=%ld sectors (%ld KB)\n",
           (long)blkdev.capacity, (long)(blkdev.capacity / 2));

    return 0;
}

/* ------------------------------------------------------------------ *
 * virtio_blk_rw — synchronous polled I/O                              *
 * ------------------------------------------------------------------ */
int virtio_blk_rw(uint64_t sector, void *buf, int write) {
    static struct virtio_blk_req req;
    static uint8_t status_byte;

    req.type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    req.reserved = 0;
    req.sector   = sector;
    status_byte  = 0xff;

    /* Descriptor 0: request header (device-readable) */
    blkdev.desc[0].addr  = (uint64_t)&req;
    blkdev.desc[0].len   = sizeof(req);
    blkdev.desc[0].flags = VIRTQ_DESC_F_NEXT;
    blkdev.desc[0].next  = 1;

    /* Descriptor 1: data buffer */
    blkdev.desc[1].addr  = (uint64_t)buf;
    blkdev.desc[1].len   = 4096;
    blkdev.desc[1].flags = VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE);
    blkdev.desc[1].next  = 2;

    /* Descriptor 2: status byte (device-writable) */
    blkdev.desc[2].addr  = (uint64_t)&status_byte;
    blkdev.desc[2].len   = 1;
    blkdev.desc[2].flags = VIRTQ_DESC_F_WRITE;
    blkdev.desc[2].next  = 0;

    /* Post to available ring */
    uint16_t avail_idx = blkdev.avail->idx;
    blkdev.avail->ring[avail_idx % VIRTQ_SIZE] = 0; /* head = descriptor 0 */
    mb();
    blkdev.avail->idx = avail_idx + 1;
    mb();

    /* Notify device: write queue index to the notify register.
     * Notify address = notify_base + queue_notify_off * notify_off_multiplier
     * (virtio spec §4.1.4.4)
     */
    volatile uint16_t *notify_reg =
        (volatile uint16_t *)(blkdev.notify_base +
                              (uint64_t)blkdev.queue_notify_off *
                              blkdev.notify_off_multiplier);
    *notify_reg = 0; /* queue index 0 */

    /* Poll for completion */
    while (blkdev.used->idx == blkdev.last_used_idx)
        mb();
    blkdev.last_used_idx++;

    if (status_byte != VIRTIO_BLK_S_OK) {
        printk("virtio_blk: I/O error, status=%d\n", status_byte);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ *
 * virtio_blk_test — write 100 × 4 KB blocks then read back and verify *
 * ------------------------------------------------------------------ */
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
                printk("virtio_blk_test: FAILED block %d byte %d: "
                       "expected 0x%x got 0x%x\n",
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

uint64_t virtio_blk_capacity(void) {
    return blkdev.capacity;
}
