#include <sys/mount.h>
#include <syscall.h>
#include <errno.h>

int mount(const char *source, const char *target,
          const char *fstype, unsigned long flags, const void *data) {
    return (int)__syscall_ret(syscall(SYS_mount, source, target, fstype, flags, data));
}

int umount2(const char *target, int flags) {
    return (int)__syscall_ret(syscall(SYS_umount2, target, flags));
}
