#define UART_THR_RBR 0x10000000
#define UART_LSR 0x10000005

#define writeaddr(addr, val) (*((volatile unsigned char *)(addr)) = (val))
#define readaddr(addr) (*((volatile unsigned char *)(addr)))

void uart_write(char bt);
char uart_read(void);