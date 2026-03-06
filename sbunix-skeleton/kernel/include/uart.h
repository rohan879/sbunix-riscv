#define UART_THR_RBR 0x10000000
#define UART_IER     0x10000001
#define UART_IER_RX  0x01
#define UART_LSR 0x10000005

#define writeaddr(addr, val) (*((volatile unsigned char *)(addr)) = (val))
#define readaddr(addr) (*((volatile unsigned char *)(addr)))

void uart_write(char bt);
void uart_intr_init(void);
int  uart_getc(void);
void uart_putc(int c);
int  uart_enqueue(char c);
int  uart_dequeue(char *c);
