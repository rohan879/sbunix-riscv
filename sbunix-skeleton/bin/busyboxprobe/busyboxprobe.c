#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <syscall.h>
#include <unistd.h>

#define SYS_prctl 167
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PAGE 4096UL

static int syscall_failed(long ret) {
    return ret < 0 && ret > -4096;
}

static int report_ret(const char *label, long ret) {
    if (syscall_failed(ret)) {
        errno = (int)-ret;
        printf("%s: FAIL ret=%ld errno=%d\n", label, ret, errno);
        return -1;
    }
    printf("%s: OK ret=0x%lx\n", label, (unsigned long)ret);
    return 0;
}

int main(void) {
    char name[16];
    memset(name, 0, sizeof(name));

    long prctl_ret = syscall(SYS_prctl, PR_GET_NAME, name, 0, 0, 0);
    if (report_ret("prctl(PR_GET_NAME)", prctl_ret) < 0)
        return 1;
    printf("task-name: %s\n", name);

    long cur_brk = syscall(SYS_brk, 0);
    if (report_ret("brk(0)", cur_brk) < 0)
        return 1;

    long grown_brk = syscall(SYS_brk, cur_brk + 0x2000);
    if (report_ret("brk(cur+0x2000)", grown_brk) < 0)
        return 1;

    long fixed_map = syscall(SYS_mmap, (void *)cur_brk, PAGE, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (report_ret("mmap(MAP_FIXED guard)", fixed_map) < 0)
        return 1;

    long anon_map = syscall(SYS_mmap, 0, PAGE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (report_ret("mmap(anon rw)", anon_map) < 0)
        return 1;

    ((volatile char *)anon_map)[0] = 'O';
    ((volatile char *)anon_map)[1] = 'K';
    ((volatile char *)anon_map)[2] = '\0';
    printf("anon-bytes: %s\n", (char *)anon_map);

    long munmap_ret = syscall(SYS_munmap, (void *)anon_map, PAGE);
    if (report_ret("munmap(anon rw)", munmap_ret) < 0)
        return 1;

    printf("[PASS] busyboxprobe\n");
    return 0;
}
