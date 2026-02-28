#ifndef _PLIC_H
#define _PLIC_H

#define PLIC_BASE              0x0c000000UL
#define PLIC_PRIORITY(source)  (PLIC_BASE + (source) * 4)
#define PLIC_SENABLE           (PLIC_BASE + 0x2080UL)
#define PLIC_STHRESHOLD        (PLIC_BASE + 0x201000UL)
#define PLIC_SCLAIM            (PLIC_BASE + 0x201004UL)
#define UART_IRQ               10

void plic_init(void);
int  plic_claim(void);
void plic_complete(int irq);

#endif
