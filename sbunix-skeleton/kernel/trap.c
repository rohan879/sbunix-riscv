#include <trap.h>
#include <printk.h>
#include <riscv.h>
#include <proc.h>
#include <syscall.h>
#include <uart.h>
#include <plic.h>

#define TIMER_FREQ 10000000UL  // 10 MHz timebase

static unsigned long boot_time;
static unsigned long ticks;
static unsigned char last_key = 0;

static inline void sbi_set_timer(unsigned long stime) {
    register unsigned long a0 asm("a0") = stime;
    register unsigned long a7 asm("a7") = 0;
    asm volatile("ecall" : : "r" (a0), "r" (a7) : "memory");
}

void timer_init(void) {
    boot_time = r_time();
    ticks = 0;
    sbi_set_timer(r_time() + TIMER_FREQ);
}

static void timer_set_next(void) {
    sbi_set_timer(r_time() + TIMER_FREQ);
}

static void print_two_digits(unsigned long n) {
    printk("%lu%lu", n / 10, n % 10);
}

static void display_key(void) {
    printk("\033[1;72H");
    if (last_key == 0) {
        printk("  ");
    } else if (last_key <= 0x1F) {
        printk("^%c", last_key + '@');
    } else if (last_key == 0x7F) {
        printk("^?");
    } else {
        printk("%c ", last_key);
    }
}

static void display_time(void) {
    unsigned long elapsed = (r_time() - boot_time) / TIMER_FREQ;
    unsigned long hours = elapsed / 3600;
    unsigned long minutes = (elapsed % 3600) / 60;
    unsigned long seconds = elapsed % 60;

    // Save cursor, move to row 1 col 60
    printk("\033[s\033[1;60H");
    print_two_digits(hours);
    printk(":");
    print_two_digits(minutes);
    printk(":");
    print_two_digits(seconds);
    display_key();
    // Restore cursor
    printk("\033[u");
}

static void handle_external_interrupt(void) {
    int irq = plic_claim();
    if (irq == UART_IRQ) {
        int c;
        while ((c = uart_getc()) != -1) {
            uart_enqueue(c);
            last_key = (unsigned char)c;
        }
        uart_wakeup();
        display_time();
        plic_complete(irq);
    } else if (irq != 0) {
        plic_complete(irq);
    }
}

static void handle_timer_interrupt(void) {
    ticks++;
    display_time();
    timer_set_next();
}

void trap_handler(struct trapframe *tf) {
    unsigned long cause = r_scause();
    unsigned long stval = r_stval();

    if (cause & SCAUSE_INTERRUPT) {
        // Interrupt
        if (cause == SCAUSE_S_TIMER_INT) {
            handle_timer_interrupt();
        } else if (cause == SCAUSE_S_EXTERNAL_INT) {
            handle_external_interrupt();
        } else {
            printk("Unknown interrupt: scause=0x%lx\n", cause);
        }
    } else {
        // Exception
        if (cause == SCAUSE_ECALL_U) {
            // Calculate instruction size
            uint16_t *instr = (uint16_t *)tf->epc;
            int size = ((*instr & 0x3) == 0x3) ? 4 : 2;

            if (tf->a7 == SYS_write) {
                char *buf = (char *)tf->a1;
                size_t len = (size_t)tf->a2;
                size_t i;
                for (i = 0; i < len; i++) {
                    printk("%c", buf[i]);
                }
                tf->a0 = (uint64_t)i;
            } else if (tf->a7 == SYS_read) {
                char *buf = (char *)tf->a1;
                size_t len = (size_t)tf->a2;
                size_t i;
                for (i = 0; i < len; ) {
                    char c;
                    /* Sleep until a byte is available in the ring buffer. */
                    while (uart_dequeue(&c) != 0) {
                        /* Nothing in buffer — yield to scheduler.
                         * The UART interrupt will call uart_wakeup() when a
                         * byte arrives, which sets us back to RUNNABLE.
                         * sret (in the scheduler's swtch path) re-enables SIE
                         * so the interrupt can actually fire. */
                        if (current_proc) {
                            current_proc->waiting_for_uart = 1;
                            current_proc->state = SLEEPING;
                            swtch(&current_proc->context, &scheduler_context);
                            /* Woken up — retry dequeue */
                        }
                    }

                    /* Translate CR -> LF so that Enter terminates the line. */
                    if (c == '\r')
                        c = '\n';

                    /* Echo the character back to the terminal. */
                    if (c == '\x7f' || c == '\x08') {
                        /* Backspace / DEL: erase previous character on terminal */
                        uart_putc('\x08');
                        uart_putc(' ');
                        uart_putc('\x08');
                    } else {
                        uart_putc(c);
                    }

                    buf[i++] = c;

                    /* For line-buffered reads, stop after newline. */
                    if (c == '\n')
                        break;
                }
                tf->a0 = (uint64_t)i;
            } else if (tf->a7 == SYS_getcwd) {
                char *buf = (char *)tf->a0;
                size_t size = (size_t)tf->a1;
                if (buf && size >= 2) {
                    buf[0] = '/';
                    buf[1] = '\0';
                }
                tf->a0 = (uint64_t)buf;
            } else if (tf->a7 == SYS_wait4) {
                tf->a0 = (uint64_t)-1;
            } else if (tf->a7 == SYS_exit || tf->a7 == SYS_exit_group) {
                int status = (int)tf->a0;
                printk("\nProcess exited with status %d. Halting CPU.\n", status);
                while(1) {}
            } else {
                printk("Unknown syscall %ld at epc 0x%lx\n", tf->a7, tf->epc);
            }

            tf->epc += size;
        } else if (cause == SCAUSE_LOAD_PAGE_FAULT || 
                   cause == SCAUSE_STORE_PAGE_FAULT ||
                   cause == SCAUSE_INST_PAGE_FAULT) {
            printk("Page fault at epc 0x%lx, stval 0x%lx, cause %lu\n", 
                   tf->epc, stval, cause);
            printk("Killing process due to page fault\n");
            while(1) {}  // halt for now
        } else {
            printk("UNEXPECTED TRAP! scause: 0x%lx, epc: 0x%lx, tval: 0x%lx\n",
                   cause, tf->epc, stval);
            while(1) {}
        }
    }
}
