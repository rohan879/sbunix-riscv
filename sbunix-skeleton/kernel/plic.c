#include <plic.h>
#include <riscv.h>

void plic_init(void) {
    // Set UART IRQ priority to 1
    *(volatile uint32_t *)PLIC_PRIORITY(UART_IRQ) = 1;

    // Enable UART IRQ in S-mode enable register (bit 10)
    *(volatile uint32_t *)PLIC_SENABLE = (1 << UART_IRQ);

    // Set S-mode priority threshold to 0 (accept all priorities > 0)
    *(volatile uint32_t *)PLIC_STHRESHOLD = 0;
}

int plic_claim(void) {
    return *(volatile uint32_t *)PLIC_SCLAIM;
}

void plic_complete(int irq) {
    *(volatile uint32_t *)PLIC_SCLAIM = irq;
}
