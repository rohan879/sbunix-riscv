/* RF: Central header for all syscall handler prototypes.
 * Part of the trap.c modular decomposition -- each handler is
 * defined in its corresponding kernel/syscall_*.c module. */
#ifndef _SYSCALL_HANDLERS_H
#define _SYSCALL_HANDLERS_H

struct trapframe;

/* Sentinel returned by handlers that set tf->a0 themselves (exit, execve) */
#define SYSCALL_NORETURN ((long)0x5BC411DEAD)

/* Total number of syscall slots (Linux RISC-V ABI upper bound) */
#define NSYSCALLS 501

extern long (*syscall_table[NSYSCALLS])(struct trapframe *tf);

/* --- syscall_io.c: file I/O, directory ops, file locking --- */
long sys_write_handler(struct trapframe *tf);
long sys_read_handler(struct trapframe *tf);
long sys_openat_handler(struct trapframe *tf);
long sys_close_handler(struct trapframe *tf);
long sys_dup3_handler(struct trapframe *tf);
long sys_getcwd_handler(struct trapframe *tf);
long sys_chdir_handler(struct trapframe *tf);
long sys_fchdir_handler(struct trapframe *tf);
long sys_lseek_handler(struct trapframe *tf);
long sys_fstat_handler(struct trapframe *tf);
long sys_newfstatat_handler(struct trapframe *tf);
long sys_getdents64_handler(struct trapframe *tf);
long sys_writev_handler(struct trapframe *tf);
long sys_readv_handler(struct trapframe *tf);
long sys_pread64_handler(struct trapframe *tf);
long sys_pwrite64_handler(struct trapframe *tf);
long sys_fcntl_handler(struct trapframe *tf);
long sys_faccessat_handler(struct trapframe *tf);
long sys_unlinkat_handler(struct trapframe *tf);
long sys_linkat_handler(struct trapframe *tf);
long sys_renameat2_handler(struct trapframe *tf);
long sys_mkdirat_handler(struct trapframe *tf);
long sys_mknodat_handler(struct trapframe *tf);
long sys_readlinkat_handler(struct trapframe *tf);
long sys_symlinkat_handler(struct trapframe *tf);
long sys_ftruncate_handler(struct trapframe *tf);
long sys_fchmod_handler(struct trapframe *tf);
long sys_fchmodat_handler(struct trapframe *tf);

/* --- syscall_process.c: clone, exec, wait, exit, process groups --- */
long sys_clone_handler(struct trapframe *tf);
long sys_execve_handler(struct trapframe *tf);
long sys_fexecve_handler(struct trapframe *tf);
long sys_wait4_handler(struct trapframe *tf);
long sys_waitid_handler(struct trapframe *tf);
long sys_exit_handler(struct trapframe *tf);
long sys_getpid_handler(struct trapframe *tf);
long sys_getppid_handler(struct trapframe *tf);
long sys_getuid_handler(struct trapframe *tf);
long sys_getgid_handler(struct trapframe *tf);
long sys_setpgid_handler(struct trapframe *tf);
long sys_setsid_handler(struct trapframe *tf);
long sys_getpgid_handler(struct trapframe *tf);
long sys_getsid_handler(struct trapframe *tf);
long sys_ps_handler(struct trapframe *tf);

/* --- syscall_memory.c: mmap, munmap, mprotect, mremap, brk --- */
long sys_brk_handler(struct trapframe *tf);
long sys_mmap_handler(struct trapframe *tf);
long sys_munmap_handler(struct trapframe *tf);
long sys_mprotect_handler(struct trapframe *tf);
long sys_mremap_handler(struct trapframe *tf);
long sys_madvise_handler(struct trapframe *tf);
long sys_msync_handler(struct trapframe *tf);

/* --- syscall_signal.c: kill, rt_sig*, tgkill, sigaltstack --- */
long sys_kill_handler(struct trapframe *tf);
long sys_rt_sigqueueinfo_handler(struct trapframe *tf);
long sys_rt_sigaction_handler(struct trapframe *tf);
long sys_rt_sigprocmask_handler(struct trapframe *tf);
long sys_rt_sigreturn_handler(struct trapframe *tf);
long sys_rt_sigsuspend_handler(struct trapframe *tf);
long sys_rt_sigpending_handler(struct trapframe *tf);
long sys_rt_sigtimedwait_handler(struct trapframe *tf);
long sys_tgkill_handler(struct trapframe *tf);
long sys_sigaltstack_handler(struct trapframe *tf);

/* --- syscall_time.c: clock, timer, nanosleep, getrusage, times --- */
long sys_clock_gettime_handler(struct trapframe *tf);
long sys_clock_getres_handler(struct trapframe *tf);
long sys_clock_settime_handler(struct trapframe *tf);
long sys_nanosleep_handler(struct trapframe *tf);
long sys_gettimeofday_handler(struct trapframe *tf);
long sys_alarm_handler(struct trapframe *tf);
long sys_timer_create_handler(struct trapframe *tf);
long sys_timer_delete_handler(struct trapframe *tf);
long sys_timer_settime_handler(struct trapframe *tf);
long sys_timer_gettime_handler(struct trapframe *tf);
long sys_timer_getoverrun_handler(struct trapframe *tf);
long sys_clock_nanosleep_handler(struct trapframe *tf);
long sys_getrusage_handler(struct trapframe *tf);
long sys_times_handler(struct trapframe *tf);

/* --- syscall_ipc.c: pipes, poll, futex, epoll, timerfd, inotify --- */
long sys_pipe2_handler(struct trapframe *tf);
long sys_ppoll_handler(struct trapframe *tf);
long sys_pselect6_handler(struct trapframe *tf);
long sys_futex_handler(struct trapframe *tf);
long sys_epoll_create1_handler(struct trapframe *tf);
long sys_epoll_ctl_handler(struct trapframe *tf);
long sys_epoll_pwait_handler(struct trapframe *tf);
long sys_timerfd_create_handler(struct trapframe *tf);
long sys_timerfd_settime_handler(struct trapframe *tf);
long sys_timerfd_gettime_handler(struct trapframe *tf);
long sys_inotify_init1_handler(struct trapframe *tf);
long sys_inotify_add_watch_handler(struct trapframe *tf);
long sys_inotify_rm_watch_handler(struct trapframe *tf);

/* --- syscall_misc.c: credentials, uname, reboot, mount, ioctl, etc. --- */
long sys_sched_yield_handler(struct trapframe *tf);
long sys_gettid_handler(struct trapframe *tf);
long sys_ioctl_handler(struct trapframe *tf);
long sys_umask_handler(struct trapframe *tf);
long sys_prctl_handler(struct trapframe *tf);
long sys_geteuid_handler(struct trapframe *tf);
long sys_getegid_handler(struct trapframe *tf);
long sys_set_tid_address_handler(struct trapframe *tf);
long sys_prlimit64_handler(struct trapframe *tf);
long sys_getrandom_handler(struct trapframe *tf);
long sys_reboot_handler(struct trapframe *tf);
long sys_fsync_handler(struct trapframe *tf);
long sys_sync_handler(struct trapframe *tf);
long sys_uname_handler(struct trapframe *tf);
long sys_mount_handler(struct trapframe *tf);
long sys_umount2_handler(struct trapframe *tf);
long sys_utimensat_handler(struct trapframe *tf);
long sys_statfs_handler(struct trapframe *tf);
long sys_fstatfs_handler(struct trapframe *tf);
long sys_fchownat_handler(struct trapframe *tf);
long sys_setuid_handler(struct trapframe *tf);
long sys_setgid_handler(struct trapframe *tf);

#endif
