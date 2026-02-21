#include <trap.h>
#include <printk.h>
#include <riscv.h>
#include <proc.h>
#include <syscall.h>
#include <uart.h>

void trap_handler(struct trapframe *tf) {
    unsigned long cause = r_scause();
    unsigned long stval = r_stval();
    
    if (cause == 8) { // User environment call
        // Calculate instruction size once
        uint16_t *instr = (uint16_t *)tf->epc;
        int size = ((*instr & 0x3) == 0x3) ? 4 : 2;

        if (tf->a7 == SYS_write) {
            // ssize_t write(int fildes, const void *buf, size_t nbyte);
            char *buf = (char *)tf->a1;
            size_t len = (size_t)tf->a2;
            size_t i;
            for (i = 0; i < len; i++) {
                printk("%c", buf[i]);
            }
            tf->a0 = (uint64_t)i; // Return number of bytes written
        } else if (tf->a7 == SYS_read) {
            // ssize_t read(int fildes, void *buf, size_t nbyte);
            char *buf = (char *)tf->a1;
            size_t len = (size_t)tf->a2;
            size_t i;
            for (i = 0; i < len; i++) {
                buf[i] = uart_read();
            }
            tf->a0 = (uint64_t)i; // Return number of bytes read
        } else if (tf->a7 == SYS_exit) {
            int status = (int)tf->a0;
            printk("\nProcess exited with status %d. Halting CPU.\n", status);
            while(1) {}
        } else {
            printk("Unknown syscall %ld at epc 0x%lx\n", tf->a7, tf->epc);
        }
        
        // Final increment
        tf->epc += size;

    } else {
        printk("UNEXPECTED TRAP! scause: 0x%lx, epc: 0x%lx, tval: 0x%lx\n", 
               cause, tf->epc, stval);
        while(1) {}
    }
}