#include <uart.h>

void uart_write(char bt) {
    // TODO can this clobber reads?
    while ((readaddr(UART_LSR) & 0x20) == 0) {
        // spin
    }
    writeaddr(UART_THR_RBR, bt);
}

char uart_read(void) {
    while ((readaddr(UART_LSR) & 0x01) == 0) {
        // spin
    }
    return readaddr(UART_THR_RBR);
}