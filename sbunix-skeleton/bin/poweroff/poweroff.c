/*
 * poweroff — halt/power off the system.
 *
 * Calls SYS_reboot with the Linux magic values and CMD_POWER_OFF.
 * Must be run as init (pid 1) or as a privileged process; in our
 * single-user simple OS any process may issue this.
 */
#include <stdio.h>
#include <syscall.h>

/* Linux reboot(2) magic numbers */
#define LINUX_REBOOT_MAGIC1   0xfee1dead
#define LINUX_REBOOT_MAGIC2   0x28121969
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedc

int main(void) {
    printf("Powering off...\n");
    syscall(SYS_reboot,
            (long)LINUX_REBOOT_MAGIC1,
            (long)LINUX_REBOOT_MAGIC2,
            (long)LINUX_REBOOT_CMD_POWER_OFF);
    /* Should not reach here */
    printf("poweroff: syscall returned unexpectedly\n");
    return 1;
}
