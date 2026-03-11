/*
 * reboot — restart the system.
 *
 * Calls SYS_reboot with the Linux magic values and CMD_RESTART.
 */
#include <stdio.h>
#include <syscall.h>

/* Linux reboot(2) magic numbers */
#define LINUX_REBOOT_MAGIC1    0xfee1dead
#define LINUX_REBOOT_MAGIC2    0x28121969
#define LINUX_REBOOT_CMD_RESTART 0x01234567

int main(void) {
    printf("Rebooting...\n");
    syscall(SYS_reboot,
            (long)LINUX_REBOOT_MAGIC1,
            (long)LINUX_REBOOT_MAGIC2,
            (long)LINUX_REBOOT_CMD_RESTART);
    /* Should not reach here */
    printf("reboot: syscall returned unexpectedly\n");
    return 1;
}
