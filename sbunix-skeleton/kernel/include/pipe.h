/* RF: Kernel pipe data structures and poll readiness interface.
 * Part of the trap.c modular decomposition -- pipe data lives in
 * syscall_ipc.c, accessed by trap.c (close_ofile) and syscall_io.c. */
#ifndef _PIPE_H
#define _PIPE_H

#define NPIPES    64
/* BF: Keep capacity at the POSIX atomic-write limit.  Larger buffers make
 * one-byte fill/drain poll workloads scale with avoidable syscall count now
 * that every user trap switches between process and kernel page tables. */
#define KPIPE_BUF 4096
/* BF: poll/select should report pipe write readiness only when a full
 * PIPE_BUF-sized atomic write can complete without blocking. */
#define KPIPE_WRITE_READY_MIN 4096

struct kpipe {
  int used;
  char buf[KPIPE_BUF];
  int rpos;        /* next read position */
  int wpos;        /* next write position */
  int write_open;  /* number of writers still open */
  int read_open;   /* number of readers still open */
};

extern struct kpipe pipes[NPIPES];

int alloc_pipe(void);
int pipe_avail(int pid);

/* Poll readiness bits */
#define KPOLLIN   0x0001
#define KPOLLOUT  0x0004
#define KPOLLERR  0x0008
#define KPOLLHUP  0x0010
#define KPOLLNVAL 0x0020

/* Forward declaration -- struct open_file defined in vfs.h */
struct open_file;
short check_fd_ready(struct open_file *f, short events);

#endif
