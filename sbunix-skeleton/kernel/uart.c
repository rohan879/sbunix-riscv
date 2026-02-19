#include <uart.h>

void uart_write(char bt) {
    // TODO can this clobber reads?
    while ((readaddr(UART_LSR) & 0x20) == 0) {
        // spin
    }
    writeaddr(UART_THR_RBR, bt);
}