#include <uart.h>
#include <riscv.h>

#define IBUF_SIZE 128

static char ibuf[IBUF_SIZE];
static int ibuf_r = 0;
static int ibuf_w = 0;

void uart_putc(int c) {
    while ((readaddr(UART_LSR) & 0x20) == 0) {
    }
    writeaddr(UART_THR_RBR, c);
}

void uart_write(char bt) {
    while ((readaddr(UART_LSR) & 0x20) == 0) {
    }
    writeaddr(UART_THR_RBR, bt);
}

int uart_enqueue(char c) {
    int next_w = (ibuf_w + 1) % IBUF_SIZE;
    if (next_w == ibuf_r) {
        return -1;
    }
    ibuf[ibuf_w] = c;
    ibuf_w = next_w;
    return 0;
}

int uart_dequeue(char *c) {
    if (ibuf_r == ibuf_w) {
        return -1;
    }
    *c = ibuf[ibuf_r];
    ibuf_r = (ibuf_r + 1) % IBUF_SIZE;
    return 0;
}

void uart_intr_init(void) {
    /* Enable RX interrupts */
    writeaddr(UART_IER, UART_IER_RX);
}

int uart_getc(void) {
    if (readaddr(UART_LSR) & 0x01)
        return readaddr(UART_THR_RBR);
    return -1;
}