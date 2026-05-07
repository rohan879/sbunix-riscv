#ifndef _PROCFS_INTERNAL_H
#define _PROCFS_INTERNAL_H

#include <proc.h>
#include <riscv.h>

/* Shared procfs entry types.  procfs.c owns lookup/VFS dispatch; the
 * renderer files own Linux-shaped pseudo-file formatting. */
enum procfs_type {
  PROCFS_ROOT,
  PROCFS_PIDDIR,
  PROCFS_STATUS,
  PROCFS_CMDLINE,
  PROCFS_FDDIR,
  PROCFS_FDLINK,
  PROCFS_MAPS,
  PROCFS_STAT_FILE,
  PROCFS_EXE_LINK,
  PROCFS_CWD_LINK,
  PROCFS_CWD_FILE,
  PROCFS_STATM,
  PROCFS_COMM,
  PROCFS_ENVIRON,
  PROCFS_ROOT_LINK,
  PROCFS_FDINFO_DIR,
  PROCFS_FDINFO_FILE,
  PROCFS_TTY_DIR,
  PROCFS_DMESG,
  PROCFS_MOUNTS,
  PROCFS_CPUINFO,
  PROCFS_VERSION,
  PROCFS_UPTIME,
  PROCFS_MEMINFO,
  PROCFS_LOADAVG,
  PROCFS_KMSG,
  PROCFS_FILESYSTEMS,
  PROCFS_CMDLINE_GLOBAL,
  PROCFS_DEVICES,
  PROCFS_PARTITIONS,
  PROCFS_CONSOLES,
  PROCFS_TTY_DRIVERS,
  PROCFS_TTY_LDISCS,
  PROCFS_STAT_GLOBAL,
  PROCFS_INTERRUPTS,
  PROCFS_SLABINFO,
  PROCFS_BUDDYINFO,
};

#define PROCFS_BUF 1024

int procfs_render_statm(char *buf, int bufsz, struct proc *p);
int procfs_render_comm(char *buf, int bufsz, struct proc *p);
int procfs_render_environ(char *buf, int bufsz, struct proc *p);
int procfs_render_fdinfo(char *buf, int bufsz, struct proc *p, int fd);

int procfs_render_global(enum procfs_type type, char *buf, int bufsz);
int procfs_global_is_dynamic(enum procfs_type type);
int procfs_global_type_from_name(const char *name, enum procfs_type *type);
uint64_t procfs_global_ino(enum procfs_type type);

extern uint64_t procfs_diag_timer_interrupts;
extern uint64_t procfs_diag_external_interrupts;
extern uint64_t procfs_diag_uart_interrupts;

#endif
