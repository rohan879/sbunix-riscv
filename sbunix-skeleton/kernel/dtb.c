#include <dtb.h>
#include <printk.h>
#include <string.h>

struct dtb_info dtb_devices;

static const char *devtype_name(enum dtb_dev_type t) {
    switch (t) {
    case DTB_DEV_UART:       return "UART";
    case DTB_DEV_PLIC:       return "PLIC";
    case DTB_DEV_CLINT:      return "CLINT";
    case DTB_DEV_VIRTIO_MMIO:return "VIRTIO_MMIO";
    case DTB_DEV_PCI:        return "PCI";
    default:                 return "UNKNOWN";
    }
}

static enum dtb_dev_type classify(const char *compat) {
    /* Check for known compatible strings */
    if (strncmp(compat, "ns16550", 7) == 0)
        return DTB_DEV_UART;
    if (strncmp(compat, "sifive,plic", 11) == 0 ||
        strncmp(compat, "riscv,plic", 10) == 0)
        return DTB_DEV_PLIC;
    if (strncmp(compat, "sifive,clint", 12) == 0 ||
        strncmp(compat, "riscv,clint", 11) == 0)
        return DTB_DEV_CLINT;
    if (strcmp(compat, "virtio,mmio") == 0)
        return DTB_DEV_VIRTIO_MMIO;
    if (strncmp(compat, "pci-host-ecam", 13) == 0)
        return DTB_DEV_PCI;
    return DTB_DEV_UNKNOWN;
}

/* Align up to 4 bytes */
static inline uint32_t align4(uint32_t x) {
    return (x + 3) & ~3;
}

void dtb_parse(void *dtb) {
    struct fdt_header *hdr = (struct fdt_header *)dtb;

    if (fdt32_to_cpu(hdr->magic) != FDT_MAGIC) {
        printk("dtb: bad magic 0x%x\n", fdt32_to_cpu(hdr->magic));
        return;
    }

    dtb_devices.ndevices = 0;

    uint32_t *p = (uint32_t *)((char *)dtb + fdt32_to_cpu(hdr->off_dt_struct));
    char *strings = (char *)dtb + fdt32_to_cpu(hdr->off_dt_strings);

    /*
     * Walk the structure block.  We track the current device entry being
     * built (cur) and add it to the table when we see FDT_END_NODE at
     * depth == 2 (direct children of /soc/).
     *
     * Depth tracking:
     *   depth 0 = root "/"
     *   depth 1 = first-level children (soc, cpus, chosen, ...)
     *   depth 2 = devices inside /soc/
     */
    int depth = 0;
    int in_soc = 0;       /* set when we're inside /soc/ */
    struct dtb_device cur;

    memset(&cur, 0, sizeof(cur));

    for (;;) {
        uint32_t token = fdt32_to_cpu(*p);
        p++;

        switch (token) {
        case FDT_BEGIN_NODE: {
            char *name = (char *)p;
            uint32_t namelen = strlen(name) + 1;
            p = (uint32_t *)((char *)p + align4(namelen));

            depth++;

            if (depth == 2 && strncmp(name, "soc", 3) == 0) {
                in_soc = 1;
            }

            if (depth == 3 && in_soc) {
                /* Starting a new device node inside /soc/ */
                memset(&cur, 0, sizeof(cur));
            }
            break;
        }

        case FDT_END_NODE: {
            if (depth == 3 && in_soc && cur.type != DTB_DEV_UNKNOWN) {
                /* Finished a device node — save it */
                if (dtb_devices.ndevices < DTB_MAX_DEVICES) {
                    dtb_devices.devices[dtb_devices.ndevices++] = cur;
                }
            }
            if (depth == 2 && in_soc) {
                in_soc = 0;
            }
            depth--;
            break;
        }

        case FDT_PROP: {
            uint32_t len = fdt32_to_cpu(*p); p++;
            uint32_t nameoff = fdt32_to_cpu(*p); p++;
            char *propname = strings + nameoff;
            char *data = (char *)p;

            if (depth == 3 && in_soc) {
                if (strcmp(propname, "compatible") == 0) {
                    cur.compatible = data;
                    cur.type = classify(data);
                } else if (strcmp(propname, "reg") == 0 && len >= 16) {
                    /* Assume #address-cells=2, #size-cells=2 */
                    uint32_t *cells = (uint32_t *)data;
                    uint64_t addr_hi = fdt32_to_cpu(cells[0]);
                    uint64_t addr_lo = fdt32_to_cpu(cells[1]);
                    uint64_t size_hi = fdt32_to_cpu(cells[2]);
                    uint64_t size_lo = fdt32_to_cpu(cells[3]);
                    cur.base_addr = (addr_hi << 32) | addr_lo;
                    cur.size = (size_hi << 32) | size_lo;
                } else if (strcmp(propname, "interrupts") == 0 && len >= 4) {
                    cur.irq = fdt32_to_cpu(*(uint32_t *)data);
                }
            }

            p = (uint32_t *)((char *)p + align4(len));
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            return;

        default:
            printk("dtb: unknown token 0x%x\n", token);
            return;
        }
    }
}

struct dtb_device *dtb_find_device(enum dtb_dev_type type) {
    return dtb_find_device_n(type, 0);
}

struct dtb_device *dtb_find_device_n(enum dtb_dev_type type, int n) {
    int count = 0;
    for (int i = 0; i < dtb_devices.ndevices; i++) {
        if (dtb_devices.devices[i].type == type) {
            if (count == n)
                return &dtb_devices.devices[i];
            count++;
        }
    }
    return NULL;
}

void dtb_print_devices(void) {
    printk("dtb: discovered %d devices:\n", dtb_devices.ndevices);
    for (int i = 0; i < dtb_devices.ndevices; i++) {
        struct dtb_device *d = &dtb_devices.devices[i];
        printk("  [%d] %-12s @ 0x%lx  size=0x%lx  irq=%d\n",
               i, devtype_name(d->type), d->base_addr, d->size, d->irq);
    }
}
