#include <trap.h>
#include <printk.h>
#include <riscv.h>
#include <proc.h>
#include <syscall.h>
#include <uart.h>
#include <plic.h>
#include <kalloc.h>
#include <string.h>
#include <vm.h>
#include <exec.h>

#define TIMER_FREQ 10000000UL  // 10 MHz timebase

static unsigned long boot_time;
static unsigned long ticks;
static unsigned char last_key = 0;

/* ------------------------------------------------------------------ */
/* Kernel pipe table                                                    */
/* ------------------------------------------------------------------ */
#define NPIPES 16
#define PIPE_BUF 4096

struct kpipe {
  int used;
  char buf[PIPE_BUF];
  int rpos;        // next read position
  int wpos;        // next write position
  int write_open;  // number of writers still open
  int read_open;   // number of readers still open
};

static struct kpipe pipes[NPIPES];

static int alloc_pipe(void) {
  for (int i = 0; i < NPIPES; i++) {
    if (!pipes[i].used) {
      pipes[i].used = 1;
      pipes[i].rpos = 0;
      pipes[i].wpos = 0;
      pipes[i].write_open = 1;
      pipes[i].read_open = 1;
      return i;
    }
  }
  return -1;
}

static int pipe_avail(int pid) {
  return pipes[pid].wpos - pipes[pid].rpos;
}

/* ------------------------------------------------------------------ */
/* File descriptor helpers                                              */
/* ------------------------------------------------------------------ */

/* Allocate a free fd slot in current_proc; returns fd or -1 */
static int alloc_fd(void) {
  for (int i = 0; i < NOFILE; i++) {
    if (!current_proc->ofile[i].used)
      return i;
  }
  return -1;
}

/* Deep copy fd table from src to dst (for clone) */
static void copy_ofile(struct proc *dst, struct proc *src) {
  for (int i = 0; i < NOFILE; i++) {
    dst->ofile[i] = src->ofile[i];
    /* bump refcounts on pipes */
    if (src->ofile[i].used) {
      int pid = src->ofile[i].pipe_id;
      if (src->ofile[i].pipe_read && pid >= 0)
        pipes[pid].read_open++;
      if (src->ofile[i].pipe_write && pid >= 0)
        pipes[pid].write_open++;
    }
  }
}

/* ------------------------------------------------------------------ */
/* SBI / timer                                                          */
/* ------------------------------------------------------------------ */

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

#ifdef TIMER_DISPLAY
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
#endif

/* ------------------------------------------------------------------ */
/* UART / interrupt                                                     */
/* ------------------------------------------------------------------ */

static void handle_external_interrupt(void) {
    int irq = plic_claim();
    if (irq == UART_IRQ) {
        int c;
        while ((c = uart_getc()) != -1) {
            if (c == 0x03) {
                /* Ctrl-C: kill all non-init foreground processes */
                int killed_any = 0;
                for (struct proc *p = proc; p < &proc[64]; p++) {
                    if (p->state != UNUSED && p->pid > 1) {
                        p->exit_status = 130; /* 128 + SIGINT */
                        p->state = ZOMBIE;
                        killed_any = 1;
                        /* Close all fds to release pipe references */
                        for (int i = 0; i < NOFILE; i++) {
                            if (p->ofile[i].used) {
                                int pid = p->ofile[i].pipe_id;
                                if (p->ofile[i].pipe_read && pid >= 0)
                                    pipes[pid].read_open--;
                                if (p->ofile[i].pipe_write && pid >= 0)
                                    pipes[pid].write_open--;
                                p->ofile[i].used = 0;
                            }
                        }
                    }
                }
                /* When children were killed the shell is blocked in waitpid.
                 * Print "^C\n" here for visual feedback, then wake all sleeping
                 * processes so waitpid returns.  Do NOT enqueue 0x03 — the shell
                 * will call read_line fresh after waitpid.
                 *
                 * When no children existed the shell is blocked in read() inside
                 * read_line.  Deliver 0x03 to read_line; read_line itself will
                 * print "^C\n" and restart the prompt (avoids a double-print). */
                if (killed_any) {
                    printk("^C\n");
                } else {
                    /* Shell was idle at prompt; deliver ^C to read_line */
                    uart_enqueue(0x03);
                }
            } else {
                uart_enqueue(c);
            }
            last_key = (unsigned char)c;
        }
        uart_wakeup();
        /* Also wake any process sleeping in wait4 so it can reap zombies.
         * Only wake waitpid sleepers (waiting_for_uart==0); do NOT wake
         * timed sleeps (waiting_for_uart==2) or UART-input sleeps (==1,
         * already handled by uart_wakeup above). */
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->state == SLEEPING && p->waiting_for_uart == 0)
                p->state = RUNNABLE;
        }
        #ifdef TIMER_DISPLAY
        display_time();
        #endif
        plic_complete(irq);
    } else if (irq != 0) {
        plic_complete(irq);
    }
}

static void handle_timer_interrupt(void) {
    ticks++;
    #ifdef TIMER_DISPLAY
    display_time();
    #endif
    timer_set_next();
    /* Wake processes whose timed sleep has expired */
    for (struct proc *p = proc; p < &proc[64]; p++) {
        if (p->state == SLEEPING && p->waiting_for_uart == 2 &&
            ticks >= p->sleep_until) {
            p->waiting_for_uart = 0;
            p->state = RUNNABLE;
        }
    }
}

/* ------------------------------------------------------------------ */
/* copy_pagetable: deep copy user pages from src to dst               */
/* ------------------------------------------------------------------ */
static int copy_pagetable(pagetable_t dst, pagetable_t src, uint64_t sz) {
    /* Walk through user virtual address space and copy PTE_U pages only */
    for (uint64_t va = 0; va < sz; va += PAGE_SIZE) {
        pte_t *pte = walk(src, va, 0);
        if (!pte || !(*pte & PTE_V))
            continue;
        /* Only copy user-accessible pages */
        if (!(*pte & PTE_U))
            continue;
        uint64_t pa = PTE2PA(*pte);
        int flags = PTE_FLAGS(*pte);

        char *mem = kalloc();
        if (!mem) return -1;
        memcpy(mem, (void *)pa, PAGE_SIZE);
        if (mappages(dst, va, PAGE_SIZE, (uint64_t)mem, flags) < 0) {
            kfree(mem);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_clone: fork the current process                                  */
/* ------------------------------------------------------------------ */
static pid_t sys_clone(void) {
    struct proc *p = allocproc();
    if (!p) return -1;

    /* Copy cwd */
    memcpy(p->cwd, current_proc->cwd, sizeof(p->cwd));

    /* Copy name */
    memcpy(p->name, current_proc->name, sizeof(p->name));

    /* Copy file descriptors */
    copy_ofile(p, current_proc);

    /* Copy trapframe — child starts after the ecall just like parent */
    memcpy(p->trapframe, current_proc->trapframe, sizeof(struct trapframe));

    /* Child returns 0 from clone/fork */
    p->trapframe->a0 = 0;

    /* Build child page table: copy kernel/device identity maps first */
    pagetable_t new_pt = uvmcreate();
    if (!new_pt) goto bad;

    /* Identity map kernel RAM and I/O (same as exec) */
    if (mappages(new_pt, 0x80000000, PHYSTOP - 0x80000000, 0x80000000,
                 PTE_R | PTE_W | PTE_X) < 0) goto bad;
    if (mappages(new_pt, 0x10000000, PAGE_SIZE, 0x10000000, PTE_R | PTE_W) < 0) goto bad;
    if (mappages(new_pt, 0x2000000, 0x10000, 0x2000000, PTE_R | PTE_W) < 0) goto bad;
    if (mappages(new_pt, 0x0c000000, 0x400000, 0x0c000000, PTE_R | PTE_W) < 0) goto bad;

    /* Copy user pages (code + data segments + stack) - PTE_U filtered */
    if (copy_pagetable(new_pt, current_proc->pagetable, MAXVA) < 0) goto bad;

    p->pagetable = new_pt;
    p->sz = current_proc->sz;
    p->parent = current_proc;

    /* Child's kernel context: resume at forkret which will sret back to user */
    p->context.ra = (uint64_t)forkret;
    p->context.sp = (uint64_t)p->kstack + PAGE_SIZE;

    p->state = RUNNABLE;
    return p->pid;

bad:
    p->state = UNUSED;
    return -1;
}

/* ------------------------------------------------------------------ */
/* sys_wait4: wait for a child to exit                                  */
/* ------------------------------------------------------------------ */
static pid_t sys_wait4(pid_t wpid, int *wstatus, int options) {
    for (;;) {
        int found_child = 0;
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->state == UNUSED) continue;
            if (p->parent != current_proc) continue;
            if (wpid > 0 && p->pid != wpid) continue;
            found_child = 1;
            if (p->state == ZOMBIE) {
                pid_t pid = p->pid;
                if (wstatus) {
                    /* encode exit status as Linux wait status */
                    *wstatus = (p->exit_status & 0xff) << 8;
                }
                /* free child resources */
                p->state = UNUSED;
                p->pid = 0;
                p->parent = NULL;
                return pid;
            }
        }
        if (!found_child) return -1;
        /* WNOHANG: don't block */
        if (options & 1) return 0;
        /* Otherwise sleep until a child exits */
        if (current_proc) {
            current_proc->waiting_for_uart = 0;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Simple in-memory "filesystem" using tar entries                      */
/* ------------------------------------------------------------------ */
#include <tar.h>
extern char _tarfs_start[];
extern char _tarfs_end[];

/* Convert an absolute/relative path to the tarfs "./..." format.
 * tarfs uses paths like "./bin/echo", "./etc/rc", etc. */
static void to_tarfs_path(const char *path, char *out) {
    /* Strip leading slash(es) */
    while (*path == '/') path++;
    if (*path == '\0') {
        out[0] = '.'; out[1] = '\0';
        return;
    }
    out[0] = '.';
    out[1] = '/';
    int i = 2;
    while (*path && i < 255) {
        out[i++] = *path++;
    }
    out[i] = '\0';
}

/* Normalize an absolute path in-place: collapse "." and ".." components.
 * Input must already be absolute (start with '/').  Result is written to out. */
static void normalize_path(const char *in, char *out) {
    /* Use a small stack of component offsets into out[] */
    /* We build the result into out directly, tracking segment starts. */
    char tmp[256];
    int tlen = 0;
    const char *p = in;

    /* Skip leading slash — we'll add it back */
    while (*p == '/') p++;

    tmp[tlen++] = '/';
    /* Stack of segment-end positions in tmp (indices of the '/' before each segment) */
    int stack[64];
    int depth = 0;

    while (*p) {
        /* Find end of this component */
        const char *start = p;
        while (*p && *p != '/') p++;
        int clen = (int)(p - start);

        if (clen == 0 || (clen == 1 && start[0] == '.')) {
            /* "." or empty — skip */
        } else if (clen == 2 && start[0] == '.' && start[1] == '.') {
            /* ".." — pop last segment */
            if (depth > 0) {
                tlen = stack[--depth];
            }
        } else {
            /* Normal component — push */
            stack[depth++] = tlen; /* save position before adding '/' + component */
            if (tlen > 1) tmp[tlen++] = '/'; /* add separator (root already has '/') */
            if (tlen + clen < 255) {
                memcpy(tmp + tlen, start, clen);
                tlen += clen;
            }
        }

        /* skip slashes between components */
        while (*p == '/') p++;
    }

    tmp[tlen] = '\0';
    /* Result: if only the root slash remains, that's "/" */
    memcpy(out, tmp, tlen + 1);
}

/* Resolve a path against cwd.  Result in out[256]. */
static void resolve_path(const char *cwd, const char *path, char *out) {
    char raw[512]; /* intermediate buffer before normalization */
    int rlen;

    if (path[0] == '/') {
        /* absolute */
        rlen = strlen(path);
        if (rlen > 511) rlen = 511;
        memcpy(raw, path, rlen);
        raw[rlen] = '\0';
    } else {
        /* relative — prepend cwd */
        int cwdlen = strlen(cwd);
        int pathlen = strlen(path);
        if (cwdlen + 1 + pathlen > 510) {
            out[0] = '/'; out[1] = '\0';
            return;
        }
        memcpy(raw, cwd, cwdlen);
        if (cwdlen == 0 || cwd[cwdlen-1] != '/') {
            raw[cwdlen++] = '/';
        }
        memcpy(raw + cwdlen, path, pathlen);
        raw[cwdlen + pathlen] = '\0';
    }

    /* Normalize: collapse "." and ".." components */
    normalize_path(raw, out);
}

/* Check if a path exists as a directory in the tarfs.
   Returns 1 if "/" or if any tar entry starts with path+"/". */
static int tar_is_dir(const char *path) {
    /* Root is always a directory */
    if (path[0] == '/' && path[1] == '\0') return 1;
    if (path[0] == '.' && path[1] == '\0') return 1;

    /* Convert to tarfs format for comparison */
    char tarpath[256];
    to_tarfs_path(path, tarpath);
    int tplen = strlen(tarpath);

    struct posix_header *hdr = (struct posix_header *)_tarfs_start;
    while ((char *)hdr < _tarfs_end) {
        if (hdr->name[0] == '\0') break;
        int nlen = strlen(hdr->name);
        /* Match: entry name == tarpath (exact) or starts with tarpath+"/" */
        if (nlen >= tplen && memcmp(hdr->name, tarpath, tplen) == 0 &&
            (hdr->name[tplen] == '/' || hdr->name[tplen] == '\0')) {
            return 1;
        }
        /* advance to next tar entry */
        unsigned long size = 0;
        for (int i = 0; i < 11; i++)
            size = size * 8 + (hdr->size[i] - '0');
        hdr = (struct posix_header *)((char *)hdr + 512 + ((size + 511) & ~511));
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main trap handler                                                    */
/* ------------------------------------------------------------------ */

void trap_handler(struct trapframe *tf) {
    unsigned long cause = r_scause();
    unsigned long stval = r_stval();

    if (cause & SCAUSE_INTERRUPT) {
        if (cause == SCAUSE_S_TIMER_INT) {
            handle_timer_interrupt();
        } else if (cause == SCAUSE_S_EXTERNAL_INT) {
            handle_external_interrupt();
        } else {
            printk("Unknown interrupt: scause=0x%lx\n", cause);
        }
        return;
    }

    /* Exception */
    if (cause != SCAUSE_ECALL_U) {
        if (cause == SCAUSE_LOAD_PAGE_FAULT ||
            cause == SCAUSE_STORE_PAGE_FAULT ||
            cause == SCAUSE_INST_PAGE_FAULT) {
            printk("Page fault pid=%d epc=0x%lx stval=0x%lx cause=%lu\n",
                   current_proc ? current_proc->pid : -1, tf->epc, stval, cause);
            /* Kill the process */
            if (current_proc) {
                current_proc->exit_status = 139;  /* SIGSEGV */
                current_proc->state = ZOMBIE;
                /* Wake parent if waiting */
                if (current_proc->parent &&
                    current_proc->parent->state == SLEEPING)
                    current_proc->parent->state = RUNNABLE;
                swtch(&current_proc->context, &scheduler_context);
            }
            while(1) {}
        }
        printk("UNEXPECTED TRAP! scause: 0x%lx, epc: 0x%lx, tval: 0x%lx\n",
               cause, tf->epc, stval);
        while(1) {}
    }

    /* ---- ecall from user ---- */
    /* Advance past the ecall instruction */
    uint16_t *instr = (uint16_t *)tf->epc;
    int size = ((*instr & 0x3) == 0x3) ? 4 : 2;
    tf->epc += size;

    long sysnum = (long)tf->a7;

    /* ---- SYS_brk: heap / program-break management ---- */
    if (sysnum == SYS_brk) {
        uint64_t new_brk = tf->a0;
        uint64_t cur_sz  = current_proc->sz;

        if (new_brk == 0) {
            /* Query: return current break */
            tf->a0 = cur_sz;
        } else if (new_brk < cur_sz) {
            /* Shrink heap (ignore for simplicity; return current brk) */
            tf->a0 = cur_sz;
        } else {
            /* Grow heap: map new pages starting from the next page boundary
             * above the current break, so we never re-map already-mapped pages.
             * Heap pages must be writable (PTE_W). */
            uint64_t heap_start = (cur_sz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t rounded    = (new_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            if (heap_start < rounded) {
                if (uvmalloc(current_proc->pagetable, heap_start, rounded, PTE_W) == 0) {
                    tf->a0 = cur_sz; /* allocation failed; return old brk */
                    goto syscall_done;
                }
            }
            current_proc->sz = new_brk;
            tf->a0 = new_brk;
        }

    } else if (sysnum == SYS_write) {
        int fd = (int)tf->a0;
        char *buf = (char *)tf->a1;
        size_t len = (size_t)tf->a2;
        if ((fd == 1 || fd == 2) &&
            !(current_proc->ofile[fd].used && current_proc->ofile[fd].pipe_write)) {
            /* stdout / stderr (only when not redirected to a pipe) */
            size_t i;
            for (i = 0; i < len; i++)
                printk("%c", buf[i]);
            tf->a0 = (uint64_t)i;
        } else if (fd >= 0 && fd < NOFILE && current_proc->ofile[fd].used &&
                   current_proc->ofile[fd].pipe_write) {
            int pid = current_proc->ofile[fd].pipe_id;
            size_t written = 0;
            while (written < len) {
                int avail = PIPE_BUF - pipe_avail(pid);
                if (avail <= 0) {
                    /* Yield and retry */
                    current_proc->state = SLEEPING;
                    swtch(&current_proc->context, &scheduler_context);
                    continue;
                }
                int n = len - written;
                if (n > avail) n = avail;
                for (int i = 0; i < n; i++) {
                    pipes[pid].buf[pipes[pid].wpos % PIPE_BUF] = buf[written + i];
                    pipes[pid].wpos++;
                }
                written += n;
                /* Wake any sleeping readers now that data is available (not timed sleeps) */
                for (struct proc *p = proc; p < &proc[64]; p++) {
                    if (p->state == SLEEPING && p != current_proc &&
                        p->waiting_for_uart != 2)
                        p->state = RUNNABLE;
                }
            }
            tf->a0 = written;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_read) {
        int fd = (int)tf->a0;
        char *buf = (char *)tf->a1;
        size_t len = (size_t)tf->a2;
        if (fd == 0 && !(current_proc->ofile[0].used && current_proc->ofile[0].pipe_read)) {
            /* stdin: UART raw byte delivery.
             * The kernel returns characters as-is; line editing (echo,
             * backspace, Ctrl-C handling) is done by the caller (sbush). */
            size_t i = 0;
            while (i < len) {
                char c;
                /* Wait until a character is available */
                while (uart_dequeue(&c) != 0) {
                    /* Poll hardware UART directly in case interrupt missed a char */
                    int hw_c = uart_getc();
                    if (hw_c != -1) {
                        uart_enqueue((char)hw_c);
                        continue;
                    }
                    /* Sleep until UART interrupt wakes us */
                    if (current_proc) {
                        current_proc->waiting_for_uart = 1;
                        current_proc->state = SLEEPING;
                        swtch(&current_proc->context, &scheduler_context);
                    }
                }
                if (c == '\r') c = '\n';
                buf[i++] = c;
                /* Return after each character so the caller can process it */
                break;
            }
            tf->a0 = (uint64_t)i;
        } else if (fd >= 0 && fd < NOFILE && current_proc->ofile[fd].used &&
                   current_proc->ofile[fd].pipe_read) {
            int pid = current_proc->ofile[fd].pipe_id;
            size_t read_bytes = 0;
            while (read_bytes < len) {
                while (pipe_avail(pid) == 0) {
                    /* Check if write end is closed */
                    if (pipes[pid].write_open == 0) goto pipe_eof;
                    current_proc->state = SLEEPING;
                    swtch(&current_proc->context, &scheduler_context);
                }
                buf[read_bytes] = pipes[pid].buf[pipes[pid].rpos % PIPE_BUF];
                pipes[pid].rpos++;
                read_bytes++;
                /* Wake any writers that might be blocked (not timed sleeps) */
                for (struct proc *p = proc; p < &proc[64]; p++) {
                    if (p->state == SLEEPING && p != current_proc &&
                        p->waiting_for_uart != 2)
                        p->state = RUNNABLE;
                }
                break; /* return what's available */
            }
pipe_eof:
            tf->a0 = (uint64_t)read_bytes;
        } else {
            /* Try reading from a tar file if the fd is a plain file */
            if (fd >= 0 && fd < NOFILE && current_proc->ofile[fd].used &&
                !current_proc->ofile[fd].pipe_read &&
                !current_proc->ofile[fd].pipe_write &&
                current_proc->ofile[fd].tar_data) {
                unsigned long off  = current_proc->ofile[fd].tar_offset;
                unsigned long fsz  = current_proc->ofile[fd].tar_size;
                unsigned long avail = (off < fsz) ? (fsz - off) : 0;
                unsigned long n    = (len < avail) ? len : avail;
                if (n > 0) {
                    memcpy(buf, current_proc->ofile[fd].tar_data + off, n);
                    current_proc->ofile[fd].tar_offset += n;
                }
                tf->a0 = (uint64_t)n;
            } else {
                tf->a0 = (uint64_t)0; /* EOF for unknown fds */
            }
        }

    } else if (sysnum == SYS_openat) {
        /* openat(AT_FDCWD, path, flags, mode) */
        const char *path = (const char *)tf->a1;
        /* int flags = (int)tf->a2; -- we only support O_RDONLY for now */

        if (!path) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        /* Resolve path against cwd, then convert to tarfs format */
        char resolved[256];
        char tarpath[256];
        resolve_path(current_proc->cwd, path, resolved);
        to_tarfs_path(resolved, tarpath);

        /* Look up in tarfs — try exact match first, then with trailing '/'
         * (tar directories are stored with a trailing slash) */
        struct posix_header *hdr = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
        if (!hdr && tarpath[0] != '\0') {
            /* Append trailing slash and retry */
            int tplen = strlen(tarpath);
            if (tplen < 254) {
                tarpath[tplen]   = '/';
                tarpath[tplen+1] = '\0';
                hdr = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
            }
        }
        if (!hdr) {
            tf->a0 = (uint64_t)-1;
        } else {
            int fd = alloc_fd();
            if (fd < 0) {
                tf->a0 = (uint64_t)-1;
            } else {
                current_proc->ofile[fd].used       = 1;
                current_proc->ofile[fd].pipe_read  = 0;
                current_proc->ofile[fd].pipe_write = 0;
                current_proc->ofile[fd].pipe_id    = -1;
                current_proc->ofile[fd].tar_offset = 0;
                current_proc->ofile[fd].tar_size   = tar_get_file_size(hdr);
                current_proc->ofile[fd].tar_data   = (char *)hdr + sizeof(struct posix_header);
                tf->a0 = (uint64_t)fd;
            }
        }

    } else if (sysnum == SYS_close) {
        int fd = (int)tf->a0;
        if (fd >= 0 && fd < NOFILE && current_proc->ofile[fd].used) {
            int pid = current_proc->ofile[fd].pipe_id;
            if (current_proc->ofile[fd].pipe_read && pid >= 0) {
                pipes[pid].read_open--;
                if (pipes[pid].read_open <= 0 && pipes[pid].write_open <= 0)
                    pipes[pid].used = 0;
                /* Wake any sleeping pipe readers/writers (not timed sleeps) */
                for (struct proc *p = proc; p < &proc[64]; p++) {
                    if (p->state == SLEEPING && p->waiting_for_uart != 2)
                        p->state = RUNNABLE;
                }
            }
            if (current_proc->ofile[fd].pipe_write && pid >= 0) {
                pipes[pid].write_open--;
                if (pipes[pid].read_open <= 0 && pipes[pid].write_open <= 0)
                    pipes[pid].used = 0;
                /* Wake any sleeping pipe readers/writers (not timed sleeps) */
                for (struct proc *p = proc; p < &proc[64]; p++) {
                    if (p->state == SLEEPING && p->waiting_for_uart != 2)
                        p->state = RUNNABLE;
                }
            }
            current_proc->ofile[fd].used = 0;
            current_proc->ofile[fd].pipe_id = -1;
            current_proc->ofile[fd].pipe_read = 0;
            current_proc->ofile[fd].pipe_write = 0;
            tf->a0 = 0;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_dup3) {
        int oldfd = (int)tf->a0;
        int newfd = (int)tf->a1;
        if (oldfd < 0 || oldfd >= NOFILE || !current_proc->ofile[oldfd].used ||
            newfd < 0 || newfd >= NOFILE) {
            tf->a0 = (uint64_t)-1;
        } else if (oldfd == newfd) {
            /* dup2(x, x) is a no-op that returns newfd */
            tf->a0 = (uint64_t)newfd;
        } else {
            /* Close newfd if open (simplified, no pipe refcount decrement) */
            if (current_proc->ofile[newfd].used) {
                current_proc->ofile[newfd].used = 0;
            }
            current_proc->ofile[newfd] = current_proc->ofile[oldfd];
            /* bump pipe refcount for the new reference */
            int pid = current_proc->ofile[oldfd].pipe_id;
            if (pid >= 0) {
                if (current_proc->ofile[oldfd].pipe_read)  pipes[pid].read_open++;
                if (current_proc->ofile[oldfd].pipe_write) pipes[pid].write_open++;
            }
            tf->a0 = (uint64_t)newfd;
        }

    } else if (sysnum == SYS_pipe2) {
        int *fds = (int *)tf->a0;
        int pid = alloc_pipe();
        if (pid < 0) {
            tf->a0 = (uint64_t)-1;
        } else {
            int rfd = alloc_fd();
            int wfd = -1;
            if (rfd >= 0) {
                current_proc->ofile[rfd].used = 1;
                current_proc->ofile[rfd].pipe_read = 1;
                current_proc->ofile[rfd].pipe_write = 0;
                current_proc->ofile[rfd].pipe_id = pid;
                wfd = alloc_fd();
            }
            if (wfd < 0) {
                if (rfd >= 0) current_proc->ofile[rfd].used = 0;
                pipes[pid].used = 0;
                tf->a0 = (uint64_t)-1;
            } else {
                current_proc->ofile[wfd].used = 1;
                current_proc->ofile[wfd].pipe_read = 0;
                current_proc->ofile[wfd].pipe_write = 1;
                current_proc->ofile[wfd].pipe_id = pid;
                fds[0] = rfd;
                fds[1] = wfd;
                tf->a0 = 0;
            }
        }

    } else if (sysnum == SYS_getcwd) {
        char *buf = (char *)tf->a0;
        size_t sz = (size_t)tf->a1;
        const char *cwd = current_proc->cwd;
        if (!cwd[0]) cwd = "/";
        int len = strlen(cwd);
        if (buf && (size_t)(len + 1) <= sz) {
            memcpy(buf, cwd, len + 1);
            tf->a0 = (uint64_t)buf;
        } else {
            tf->a0 = (uint64_t)0;
        }

    } else if (sysnum == SYS_chdir) {
        const char *path = (const char *)tf->a0;
        if (!path) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }
        char resolved[256];
        resolve_path(current_proc->cwd, path, resolved);
        if (tar_is_dir(resolved)) {
            memcpy(current_proc->cwd, resolved, sizeof(current_proc->cwd));
            tf->a0 = 0;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_clone) {
        /* fork() is implemented as clone(SIGCHLD, 0, 0, 0, 0) */
        pid_t child = sys_clone();
        tf->a0 = (uint64_t)child;

    } else if (sysnum == SYS_execve) {
        const char *path = (const char *)tf->a0;
        char **argv = (char **)tf->a1;
        /* char **envp = (char **)tf->a2; -- ignored for now */

        if (!path) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        /* ---- Copy argv strings into kernel-heap buffers BEFORE exec() ----
         * argv pointers live in the old user address space.  After exec()
         * replaces the page table those VA→PA mappings no longer exist.
         * We allocate a 4K page for string data to avoid kernel stack overflow. */
        #define EXECVE_MAXARGS  32

        /* Use kalloc() for the large string buffer to avoid stack overflow */
        char *kstrbuf = (char *)kalloc();
        if (!kstrbuf) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        int kargc = 0;
        int kstroff = 0;
        int kstroff_arr[EXECVE_MAXARGS];

        if (argv) {
            while (kargc < EXECVE_MAXARGS && argv[kargc]) {
                const char *s = argv[kargc];
                int slen = strlen(s) + 1;
                if (kstroff + slen > (int)PAGE_SIZE) break;
                kstroff_arr[kargc] = kstroff;
                memcpy(kstrbuf + kstroff, s, slen);
                kstroff += slen;
                kargc++;
            }
        }

        /* Also copy the path before exec() changes the address space */
        char kpath[256];
        {
            int plen = strlen(path);
            if (plen > 255) plen = 255;
            memcpy(kpath, path, plen + 1);
        }

        /* Resolve path relative to cwd, then convert to tarfs format */
        char resolved[256];
        char tarpath[256];
        resolve_path(current_proc->cwd, kpath, resolved);
        to_tarfs_path(resolved, tarpath);

        int ret = exec(tarpath);
        if (ret < 0) {
            char orig_tarpath[256];
            to_tarfs_path(kpath, orig_tarpath);
            ret = exec(orig_tarpath);
        }
        if (ret < 0) {
            ret = exec(kpath);
        }
        if (ret < 0) {
            kfree(kstrbuf);
            tf->a0 = (uint64_t)-1;
            return;
        }

        /* ---- Set up the new user stack with argc/argv ---- */
        /* Stack layout at sp (growing down from MAXVA-32):
         *   sp+0:              argc (8 bytes)
         *   sp+8:              argv[0..argc-1] pointers ((argc+1)*8 bytes)
         *   sp+8+(argc+1)*8:   string data (kstroff bytes)
         */
        {
            struct trapframe *newtf = current_proc->trapframe;

            int ptr_area = (kargc + 1) * 8;
            int header   = 8;
            int total    = header + ptr_area + kstroff;
            total = (total + 15) & ~15;

            uint64_t sp       = MAXVA - 32 - total;
            uint64_t str_base = sp + header + ptr_area;
            uint64_t ptr_base = sp + header;

            /* Use kalloc for kbuf too to avoid stack overflow */
            char *kbuf = (char *)kalloc();
            if (kbuf) {
                int off = 0;
                /* argc */
                uint64_t u_argc = (uint64_t)kargc;
                memcpy(kbuf + off, &u_argc, 8); off += 8;

                /* argv pointers */
                uint64_t argv_ptrs[EXECVE_MAXARGS + 1];
                int str_off2 = 0;
                for (int i = 0; i < kargc; i++) {
                    argv_ptrs[i] = str_base + str_off2;
                    int slen = strlen(kstrbuf + kstroff_arr[i]) + 1;
                    str_off2 += slen;
                }
                argv_ptrs[kargc] = 0;
                memcpy(kbuf + off, argv_ptrs, (kargc + 1) * 8);
                off += (kargc + 1) * 8;

                /* string data */
                if (kstroff > 0) {
                    memcpy(kbuf + off, kstrbuf, kstroff);
                    off += kstroff;
                }

                uvmcopyin(current_proc->pagetable, sp, kbuf, off);
                kfree(kbuf);
            }

            newtf->sp = sp;
            newtf->a0 = (uint64_t)kargc;
            newtf->a1 = ptr_base;
        }

        kfree(kstrbuf);

        /* exec updated the trapframe; trap_restore will sret into new process */
        trap_restore(current_proc->trapframe);
        /* never reached */

    } else if (sysnum == SYS_wait4) {
        pid_t wpid   = (pid_t)(int)tf->a0;
        int *wstatus = (int *)tf->a1;
        int options  = (int)tf->a2;
        tf->a0 = (uint64_t)sys_wait4(wpid, wstatus, options);

    } else if (sysnum == SYS_getpid) {
        tf->a0 = current_proc ? (uint64_t)current_proc->pid : 0;

    } else if (sysnum == SYS_getppid) {
        if (current_proc && current_proc->parent)
            tf->a0 = (uint64_t)current_proc->parent->pid;
        else
            tf->a0 = 1; /* init is own parent by convention */

    } else if (sysnum == SYS_getuid) {
        tf->a0 = 0; /* always root */

    } else if (sysnum == SYS_getgid) {
        tf->a0 = 0; /* always root */

    } else if (sysnum == SYS_kill) {
        pid_t target = (pid_t)(int)tf->a0;
        /* int sig = (int)tf->a1; -- only SIGKILL is meaningful here */
        if (target <= 0) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }
        int found = 0;
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->state != UNUSED && p->pid == target) {
                p->exit_status = 128 + 9; /* killed by SIGKILL */
                p->state = ZOMBIE;
                if (p->parent && p->parent->state == SLEEPING)
                    p->parent->state = RUNNABLE;
                found = 1;
                break;
            }
        }
        tf->a0 = found ? 0 : (uint64_t)-1;

    } else if (sysnum == SYS_clock_gettime) {
        /* clock_gettime(clockid_t, struct timespec *)
         * struct timespec: { int64_t tv_sec; int64_t tv_nsec; }
         * We report ticks as seconds (each tick = 1s). */
        int64_t *ts = (int64_t *)tf->a1;
        if (ts) {
            ts[0] = (int64_t)ticks;   /* tv_sec */
            ts[1] = 0;                 /* tv_nsec */
            tf->a0 = 0;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_lseek) {
        int fd      = (int)tf->a0;
        long offset = (long)tf->a1;
        int whence  = (int)tf->a2;
        if (fd < 0 || fd >= NOFILE || !current_proc->ofile[fd].used ||
            current_proc->ofile[fd].pipe_read || current_proc->ofile[fd].pipe_write) {
            tf->a0 = (uint64_t)-1;
        } else {
            unsigned long fsz = current_proc->ofile[fd].tar_size;
            unsigned long cur = current_proc->ofile[fd].tar_offset;
            long new_off;
            if (whence == 0)       /* SEEK_SET */ new_off = offset;
            else if (whence == 1)  /* SEEK_CUR */ new_off = (long)cur + offset;
            else if (whence == 2)  /* SEEK_END */ new_off = (long)fsz + offset;
            else { tf->a0 = (uint64_t)-1; goto syscall_done; }
            if (new_off < 0) new_off = 0;
            if ((unsigned long)new_off > fsz) new_off = (long)fsz;
            current_proc->ofile[fd].tar_offset = (unsigned long)new_off;
            tf->a0 = (uint64_t)new_off;
        }

    } else if (sysnum == SYS_fstat) {
        /* fstat(int fd, struct stat *st)
         * Minimal struct stat layout (RISC-V Linux ABI, first fields):
         *   0:  uint64_t st_dev
         *   8:  uint64_t st_ino
         *  16:  uint32_t st_mode
         *  20:  uint32_t st_nlink
         *  24:  uint32_t st_uid
         *  28:  uint32_t st_gid
         *  32:  uint64_t st_rdev
         *  40:  int64_t  pad0
         *  48:  int64_t  st_size
         *  56:  int32_t  st_blksize
         *  60:  int32_t  pad1
         *  64:  int64_t  st_blocks
         */
        int fd = (int)tf->a0;
        uint8_t *st = (uint8_t *)tf->a1;
        if (!st || fd < 0 || fd >= NOFILE || !current_proc->ofile[fd].used) {
            tf->a0 = (uint64_t)-1;
        } else {
            memset(st, 0, 128); /* zero the whole stat struct */
            unsigned long fsz = current_proc->ofile[fd].tar_size;
            /* st_mode: S_IFREG | 0644 = 0x81A4 */
            uint32_t mode = 0x81A4;
            uint32_t nlink = 1;
            int64_t  size  = (int64_t)fsz;
            int32_t  blksz = 512;
            int64_t  blks  = ((int64_t)fsz + 511) / 512;
            memcpy(st + 16, &mode,  4);
            memcpy(st + 20, &nlink, 4);
            memcpy(st + 48, &size,  8);
            memcpy(st + 56, &blksz, 4);
            memcpy(st + 64, &blks,  8);
            tf->a0 = 0;
        }

    } else if (sysnum == SYS_newfstatat) {
        /* newfstatat(AT_FDCWD, path, struct stat *, flags) */
        const char *path = (const char *)tf->a1;
        uint8_t *st = (uint8_t *)tf->a2;
        if (!path || !st) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }
        char resolved[256], tarpath[256];
        resolve_path(current_proc->cwd, path, resolved);
        to_tarfs_path(resolved, tarpath);
        struct posix_header *hdr = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
        if (!hdr) {
            /* retry with trailing '/' for directories stored as "path/" in tarfs */
            int tplen = strlen(tarpath);
            if (tplen > 0 && tplen < 254 && tarpath[tplen - 1] != '/') {
                tarpath[tplen]     = '/';
                tarpath[tplen + 1] = '\0';
                hdr = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
            }
        }
        if (!hdr) {
            tf->a0 = (uint64_t)-1;
        } else {
            memset(st, 0, 128);
            unsigned long fsz = tar_get_file_size(hdr);
            uint32_t mode = (hdr->typeflag == '5') ? (uint32_t)(0x4000 | 0755)
                                                    : (uint32_t)(0x8000 | 0644);
            uint32_t nlink = 1;
            int64_t  size  = (int64_t)fsz;
            int32_t  blksz = 512;
            int64_t  blks  = ((int64_t)fsz + 511) / 512;
            memcpy(st + 16, &mode,  4);
            memcpy(st + 20, &nlink, 4);
            memcpy(st + 48, &size,  8);
            memcpy(st + 56, &blksz, 4);
            memcpy(st + 64, &blks,  8);
            tf->a0 = 0;
        }

    } else if (sysnum == SYS_getdents64) {
        /* getdents64(int fd, struct linux_dirent64 *buf, unsigned int count)
         * struct linux_dirent64:
         *   uint64_t d_ino
         *   int64_t  d_off
         *   uint16_t d_reclen
         *   uint8_t  d_type
         *   char     d_name[]  (NUL-terminated, padded to 8-byte boundary)
         */
        int fd            = (int)tf->a0;
        char *ubuf        = (char *)tf->a1;
        unsigned int count = (unsigned int)tf->a2;

        /* fd must be an open "directory" — we accept any tarfs-backed fd
         * whose tar_data points at a directory header, OR we treat the
         * fd itself as a directory cursor by storing a scan position.
         * Simplest approach: we repurpose tar_offset as "bytes of dirents
         * already returned" and scan the tarfs from scratch each time,
         * skipping entries we already emitted. */

        if (fd < 0 || fd >= NOFILE || !current_proc->ofile[fd].used ||
            current_proc->ofile[fd].pipe_read || current_proc->ofile[fd].pipe_write) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        /* We need to know which directory path this fd refers to.
         * We stored tar_data pointing to the header; for a directory entry
         * the header name IS the directory path. For now, walk the tarfs
         * and find entries whose parent directory matches. */

        /* Recover the directory path from the stored tar header */
        struct posix_header *dir_hdr = (struct posix_header *)
            (current_proc->ofile[fd].tar_data - sizeof(struct posix_header));
        /* Compute the canonical dir path from the tar name (strip "./", ensure trailing /) */
        char dirpath[256];
        {
            const char *n = dir_hdr->name;
            if (n[0] == '.' && n[1] == '/') n += 2;
            /* strip trailing slash */
            int nl = strlen(n);
            if (nl > 0 && n[nl-1] == '/') nl--;
            memcpy(dirpath, n, nl);
            dirpath[nl] = '\0';
        }
        int dirpath_len = strlen(dirpath);

        unsigned long skip = current_proc->ofile[fd].tar_offset;
        unsigned int  written = 0;
        unsigned long seen    = 0;

        /* Helper: emit one dirent into ubuf.  Returns 1 on success, 0 if no room. */
#define EMIT_DIRENT(name_, ino_, dtype_) do { \
    int _nl = strlen(name_); \
    int _rl = (19 + _nl + 1 + 7) & ~7; \
    if (written + _rl > count) goto getdents_done; \
    char *_d = ubuf + written; \
    memset(_d, 0, _rl); \
    uint64_t _ino = (uint64_t)(ino_); \
    int64_t  _off = (int64_t)(seen + 1); \
    uint16_t _rlen = (uint16_t)_rl; \
    uint8_t  _dt   = (uint8_t)(dtype_); \
    memcpy(_d +  0, &_ino,  8); \
    memcpy(_d +  8, &_off,  8); \
    memcpy(_d + 16, &_rlen, 2); \
    memcpy(_d + 18, &_dt,   1); \
    memcpy(_d + 19, (name_), _nl + 1); \
    written += _rl; \
    current_proc->ofile[fd].tar_offset = seen + 1; \
} while (0)

        /* Synthetic "." entry (seen index 0) */
        if (seen >= skip) {
            uint64_t dot_ino = (uint64_t)((char *)dir_hdr - _tarfs_start) / 512 + 1;
            EMIT_DIRENT(".", dot_ino, 4 /* DT_DIR */);
        }
        seen++;

        /* Synthetic ".." entry (seen index 1) */
        if (seen >= skip) {
            /* Find parent directory header for inode number */
            uint64_t dotdot_ino = 1; /* default: root */
            if (dirpath_len > 0) {
                /* Strip last component from dirpath to get parent path */
                char parentpath[256];
                int pl = dirpath_len - 1;
                while (pl > 0 && dirpath[pl] != '/') pl--;
                if (pl == 0) {
                    /* Parent is root — its tar entry is "./" */
                    parentpath[0] = '\0';
                } else {
                    memcpy(parentpath, dirpath, pl);
                    parentpath[pl] = '\0';
                }
                /* Search tarfs for the parent header */
                struct posix_header *ph = (struct posix_header *)_tarfs_start;
                while ((char *)ph < _tarfs_end) {
                    if (ph->name[0] == '\0') break;
                    if (strncmp(ph->magic, "ustar", 5) == 0) {
                        const char *pn = ph->name;
                        if (pn[0] == '.' && pn[1] == '/') pn += 2;
                        int pnl = strlen(pn);
                        if (pnl > 0 && pn[pnl-1] == '/') pnl--;
                        if (pnl == (int)strlen(parentpath) &&
                            memcmp(pn, parentpath, pnl) == 0) {
                            dotdot_ino = (uint64_t)((char *)ph - _tarfs_start) / 512 + 1;
                            break;
                        }
                    }
                    unsigned long fsz3 = tar_get_file_size(ph);
                    ph = (struct posix_header *)((char *)ph + 512 + ((fsz3 + 511) & ~511UL));
                }
            }
            EMIT_DIRENT("..", dotdot_ino, 4 /* DT_DIR */);
        }
        seen++;

        struct posix_header *hdr = (struct posix_header *)_tarfs_start;
        while ((char *)hdr < _tarfs_end && written + 32 <= count) {
            if (hdr->name[0] == '\0') break;
            if (strncmp(hdr->magic, "ustar", 5) != 0) {
                hdr = (struct posix_header *)((char *)hdr + 512);
                continue;
            }

            /* Strip "./" prefix from tar name */
            const char *ename = hdr->name;
            if (ename[0] == '.' && ename[1] == '/') ename += 2;

            /* Remove trailing slash for comparison */
            char clean[100];
            int elen = strlen(ename);
            if (elen > 0 && ename[elen-1] == '/') elen--;
            memcpy(clean, ename, elen);
            clean[elen] = '\0';

            /* Check if this entry's parent is dirpath */
            int is_child = 0;
            if (dirpath_len == 0) {
                /* Root directory: direct children have no '/' in clean */
                is_child = (elen > 0 && strchr(clean, '/') == (void *)0);
            } else {
                /* Child if clean starts with "dirpath/" and has no further '/' */
                if (elen > dirpath_len + 1 &&
                    memcmp(clean, dirpath, dirpath_len) == 0 &&
                    clean[dirpath_len] == '/') {
                    const char *rest = clean + dirpath_len + 1;
                    is_child = (strchr(rest, '/') == (void *)0 && rest[0] != '\0');
                }
            }

            if (is_child) {
                if (seen >= skip) {
                    /* Emit this dirent */
                    const char *basename = clean + (dirpath_len > 0 ? dirpath_len + 1 : 0);
                    int namelen = strlen(basename);
                    /* reclen = 8+8+2+1+1 + namelen + 1, rounded to 8 */
                    int reclen = (19 + namelen + 1 + 7) & ~7;
                    if (written + reclen > count) break;

                    char *d = ubuf + written;
                    memset(d, 0, reclen);
                    uint64_t ino  = (uint64_t)((char *)hdr - _tarfs_start) / 512 + 1;
                    int64_t  doff = (int64_t)(seen + 1);
                    uint16_t rlen = (uint16_t)reclen;
                    uint8_t  dtype = (hdr->typeflag == '5') ? 4 : 8; /* DT_DIR=4, DT_REG=8 */
                    memcpy(d +  0, &ino,   8);
                    memcpy(d +  8, &doff,  8);
                    memcpy(d + 16, &rlen,  2);
                    memcpy(d + 18, &dtype, 1);
                    memcpy(d + 19, basename, namelen + 1);
                    written += reclen;
                    current_proc->ofile[fd].tar_offset = seen + 1;
                }
                seen++;
            }

            /* Advance to next tar entry */
            unsigned long fsz2 = tar_get_file_size(hdr);
            hdr = (struct posix_header *)((char *)hdr + 512 + ((fsz2 + 511) & ~511UL));
        }
getdents_done:
        tf->a0 = (uint64_t)written;

    } else if (sysnum == SYS_nanosleep) {
        /* nanosleep(const struct timespec *req, struct timespec *rem)
         * struct timespec: { uint64_t tv_sec; uint64_t tv_nsec; }
         * We sleep for tv_sec seconds (ignoring tv_nsec for simplicity).
         * One timer tick = 1 second (TIMER_FREQ = 10 MHz). */
        uint64_t *req = (uint64_t *)tf->a0;
        if (req && current_proc) {
            uint64_t secs = req[0]; /* tv_sec */
            current_proc->sleep_until     = ticks + secs;
            current_proc->waiting_for_uart = 2;
            current_proc->state           = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
        }
        tf->a0 = 0;

    } else if (sysnum == SYS_exit || sysnum == SYS_exit_group) {
        int status = (int)tf->a0;
        if (current_proc) {
            current_proc->exit_status = status;
            /* Close all open fds */
            for (int i = 0; i < NOFILE; i++) {
                if (current_proc->ofile[i].used) {
                    int pid = current_proc->ofile[i].pipe_id;
                    if (current_proc->ofile[i].pipe_read && pid >= 0) {
                        pipes[pid].read_open--;
                        if (pipes[pid].read_open <= 0 && pipes[pid].write_open <= 0)
                            pipes[pid].used = 0;
                    }
                    if (current_proc->ofile[i].pipe_write && pid >= 0) {
                        pipes[pid].write_open--;
                        if (pipes[pid].read_open <= 0 && pipes[pid].write_open <= 0)
                            pipes[pid].used = 0;
                    }
                    current_proc->ofile[i].used = 0;
                }
            }
            /* If init (pid 1) exits, the system has nothing left to run */
            if (current_proc->pid == 1) {
                printk("\nSystem halted (init exited with status %d).\n", status);
                /* Use SBI shutdown (extension 0x53525354 "SRST", function 0) */
                register unsigned long a0 asm("a0") = 0; /* shutdown */
                register unsigned long a1 asm("a1") = 0;
                register unsigned long a7 asm("a7") = 0x53525354;
                asm volatile("ecall" : : "r"(a0), "r"(a1), "r"(a7));
                while (1) {}
            }
            current_proc->state = ZOMBIE;
            /* Wake parent if it's waiting */
            if (current_proc->parent &&
                current_proc->parent->state == SLEEPING) {
                current_proc->parent->state = RUNNABLE;
            }
            /* Also wake any process waiting in wait4 (waiting_for_uart==0).
             * Do NOT wake timed-sleep processes (waiting_for_uart==2). */
            for (struct proc *p = proc; p < &proc[64]; p++) {
                if (p->state == SLEEPING && p != current_proc &&
                    p->waiting_for_uart == 0)
                    p->state = RUNNABLE;
            }
            swtch(&current_proc->context, &scheduler_context);
        }
        while(1) {}

    } else if (sysnum == SYS_mmap) {
        /* mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
         * Only MAP_ANONYMOUS supported.  Returns VA on success, (uint64_t)-1 on failure. */
        uint64_t addr  = tf->a0;
        uint64_t len   = tf->a1;
        /* int prot  = (int)tf->a2; -- ignored beyond PROT_READ|PROT_WRITE */
        int      flags = (int)tf->a3;
        /* int fd = (int)tf->a4;  -- must be -1 for MAP_ANONYMOUS */
        /* off_t off = tf->a5;    -- must be 0 for MAP_ANONYMOUS */

        /* Round len up to page boundary */
        len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        if (len == 0) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        /* Choose virtual address */
        uint64_t va;
        if (flags & MAP_FIXED) {
            va = addr & ~(uint64_t)(PAGE_SIZE - 1);
        } else {
            /* Lazily initialise mmap_base to a safe region above the heap */
            if (current_proc->mmap_base == 0)
                current_proc->mmap_base = 0x20000000UL; /* 512 MB */
            va = current_proc->mmap_base;
            current_proc->mmap_base += len;
        }

        /* Allocate a free slot in mmap_regions[] */
        int slot = -1;
        for (int i = 0; i < NMMAP; i++) {
            if (current_proc->mmap_regions[i].va == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        /* Map zeroed pages */
        if (uvmalloc(current_proc->pagetable, va, va + len, PTE_W) == 0) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        current_proc->mmap_regions[slot].va  = va;
        current_proc->mmap_regions[slot].len = len;
        tf->a0 = va;

    } else if (sysnum == SYS_munmap) {
        /* munmap(void *addr, size_t len) */
        uint64_t addr = tf->a0;
        uint64_t len  = tf->a1;

        len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

        /* Find matching region */
        int found = 0;
        for (int i = 0; i < NMMAP; i++) {
            if (current_proc->mmap_regions[i].va == addr) {
                uvmunmap(current_proc->pagetable, addr, current_proc->mmap_regions[i].len);
                sfence_vma();
                current_proc->mmap_regions[i].va  = 0;
                current_proc->mmap_regions[i].len = 0;
                found = 1;
                break;
            }
        }
        tf->a0 = found ? 0 : (uint64_t)-1;

    } else if (sysnum == SYS_mprotect) {
        /* mprotect stub — always succeed */
        tf->a0 = 0;

    } else if (sysnum == SYS_ps) {
        /* ps(struct ps_entry *buf, int maxentries) -> number of entries written
         * struct ps_entry { int32_t pid; int32_t ppid; char state; char name[15]; }
         * = 20 bytes, padded to 24 for alignment. */
        uint8_t *buf      = (uint8_t *)tf->a0;
        int      maxn     = (int)tf->a1;
        int      written  = 0;
        if (buf && maxn > 0) {
            for (struct proc *p = proc; p < &proc[64]; p++) {
                if (p->state == UNUSED) continue;
                if (written >= maxn) break;
                /* encode state as single letter */
                char st;
                switch (p->state) {
                    case SLEEPING:  st = 'S'; break;
                    case RUNNABLE:  st = 'R'; break;
                    case RUNNING:   st = 'R'; break;
                    case ZOMBIE:    st = 'Z'; break;
                    default:        st = '?'; break;
                }
                uint8_t *e = buf + written * 24;
                int32_t pid_  = (int32_t)p->pid;
                int32_t ppid_ = (int32_t)(p->parent ? p->parent->pid : 0);
                memset(e, 0, 24);
                memcpy(e + 0, &pid_,  4);
                memcpy(e + 4, &ppid_, 4);
                e[8] = (uint8_t)st;
                /* copy name, max 15 chars */
                int nl = strlen(p->name);
                if (nl > 15) nl = 15;
                memcpy(e + 9, p->name, nl);
                written++;
            }
        }
        tf->a0 = (uint64_t)written;

    } else if (sysnum == SYS_ioctl) {
        int fd = (int)tf->a0;
        unsigned long request = tf->a1;
        uint64_t argp = tf->a2;

        if (request == 0x5413) { /* TIOCGWINSZ */
            if (fd >= 0 && fd <= 2) {
                /* stdin/stdout/stderr are the UART terminal */
                if (argp) {
                    /* Write a default 80x24 winsize struct to user memory */
                    struct { uint16_t row, col, xpix, ypix; } ws = {24, 80, 0, 0};
                    pte_t *pte = walk(current_proc->pagetable, argp, 0);
                    if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                        uint64_t pa = PTE2PA(*pte) + (argp & 0xFFF);
                        memcpy((void *)pa, &ws, sizeof(ws));
                    }
                }
                tf->a0 = 0; /* success - it IS a terminal */
            } else {
                tf->a0 = (uint64_t)-25; /* -ENOTTY */
            }
        } else if (request == 0x540F) { /* TIOCGPGRP */
            tf->a0 = (uint64_t)current_proc->pid;
        } else if (request == 0x5410) { /* TIOCSPGRP */
            tf->a0 = 0;
        } else {
            tf->a0 = (uint64_t)-25; /* -ENOTTY for unknown ioctls */
        }

    } else {
        printk("Unknown syscall %ld at epc 0x%lx\n", sysnum, tf->epc - size);
        tf->a0 = (uint64_t)-1;
    }
syscall_done:
    return;
}
