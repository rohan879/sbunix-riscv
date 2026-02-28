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
    // Restore cursor
    printk("\033[u");
}

static void handle_external_interrupt(void) {
    int irq = plic_claim();
    if (irq == UART_IRQ) {
        int c;
        while ((c = uart_getc()) != -1) {
            // Consume received characters
            (void)c;
        }
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
                for (i = 0; i < len; i++) {
                    buf[i] = uart_read();
                }
                tf->a0 = (uint64_t)i;
            } else if (tf->a7 == SYS_exit) {
                int status = (int)tf->a0;
                printk("\nProcess exited with status %d. Halting CPU.\n", status);
                while(1) {}
            } else {
                printk("Unknown syscall %ld at epc 0x%lx\n", tf->a7, tf->epc);
            }

            tf->epc += size;
        } else {
            printk("UNEXPECTED TRAP! scause: 0x%lx, epc: 0x%lx, tval: 0x%lx\n",
                   cause, tf->epc, stval);
            while(1) {}
        }
    }
}
