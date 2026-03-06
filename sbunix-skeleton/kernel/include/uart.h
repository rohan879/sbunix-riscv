#define UART_THR_RBR 0x10000000
#define UART_IER     0x10000001
#define UART_IER_RX  0x01
#define UART_FCR     0x10000002  /* FIFO Control Register */
#define UART_FCR_FIFO_EN   0x01  /* Enable FIFO */
#define UART_FCR_RX_RESET  0x02  /* Reset RX FIFO */
#define UART_FCR_TX_RESET  0x04  /* Reset TX FIFO */
#define UART_FCR_TRIG_1    0x00  /* Interrupt trigger: 1 byte */
#define UART_LSR 0x10000005

#define writeaddr(addr, val) (*((volatile unsigned char *)(addr)) = (val))
#define readaddr(addr) (*((volatile unsigned char *)(addr)))

void uart_write(char bt);
void uart_intr_init(void);
int  uart_getc(void);
void uart_putc(int c);
int  uart_enqueue(char c);
int  uart_dequeue(char *c);
