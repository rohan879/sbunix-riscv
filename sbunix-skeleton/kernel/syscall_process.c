/* RF: Process management syscall handlers extracted from trap.c.
 * Clone, exec, wait, exit, process groups, CoW page table copy. */

#include <syscall_handlers.h>
#include <syscall.h>
#include <trap_internal.h>
#include <pathres.h>
#include <pipe.h>
#include <proc.h>
#include <exec.h>
#include <vm.h>
#include <mm.h>
#include <kalloc.h>
#include <vfs.h>
#include <ext2.h>
#include <riscv.h>
#include <string.h>
#include <printk.h>
#include <swap.h>
#include <tar.h>
#include <kmalloc.h>

extern char _tarfs_start[];
extern char _tarfs_end[];

/* ------------------------------------------------------------------ */
/* copy_pagetable_cow: COW-share user pages from src to dst           */
/* ------------------------------------------------------------------ */
/* RF: Made non-static for access by extracted process syscall module */
int copy_pagetable_cow(pagetable_t dst, pagetable_t src,
                       uint64_t start, uint64_t end) {
    /* Walk a specific VA range and COW-share PTE_U pages.
     * Super page PTEs (level-1 leaf, detected by PTE_IS_LEAF) are skipped
     * by advancing VA to the next 2MB boundary instead of 4KB. */
    for (uint64_t va = start; va < end; ) {
        pte_t *pte = walk(src, va, 0);
        if (!pte) {
            va += PAGE_SIZE;
            continue;
        }

        /* Detect super page (level-1 leaf with R/W/X set).  These are
         * kernel I/O or RAM mappings — skip the entire 2MB region.
         * Without this, walk(dst, va, 1) would allocate level-0 page
         * table pages INSIDE the child's super page region, corrupting
         * the child's kernel mapping. */
        if ((*pte & PTE_V) && PTE_IS_LEAF(*pte) && !(*pte & PTE_U)) {
            va = (va & ~(SUPERPAGE_SIZE - 1)) + SUPERPAGE_SIZE;
            continue;
        }

        /* Handle swapped-out pages: duplicate the swap slot */
        if (PTE_IS_SWAPPED(*pte)) {
            int old_slot = PTE_SWAP_SLOT(*pte);
            int new_slot = swap_alloc();
            if (new_slot < 0) return -1; /* swap full */

            char *tmp = kalloc();
            if (!tmp) {
                swap_free(new_slot);
                return -1;
            }
            swap_read(old_slot, tmp);
            swap_write(new_slot, tmp);
            kfree(tmp);

            /* Child gets new slot with same flags */
            pte_t child_pte = PTE_SWAP_ENCODE(new_slot, PTE_SWAP_FLAGS(*pte));
            pte_t *cpte = walk(dst, va, 1);
            if (!cpte) {
                swap_free(new_slot);
                return -1;
            }
            *cpte = child_pte;
            va += PAGE_SIZE;
            continue;
        }

        if (!(*pte & PTE_V)) {
            va += PAGE_SIZE;
            continue;
        }
        /* Only share user-accessible pages */
        if (!(*pte & PTE_U)) {
            va += PAGE_SIZE;
            continue;
        }

        uint64_t pa = PTE2PA(*pte);
        int flags = PTE_FLAGS(*pte);

        int orig_flags = PTE_FLAGS(*pte);  /* BF: save before CoW conversion for rollback */

        if (flags & PTE_W) {
            /* Writable page: mark read-only + COW in both parent and child */
            *pte = PA2PTE(pa) | ((flags & ~PTE_W) | PTE_COW | PTE_V);
            flags = (flags & ~PTE_W) | PTE_COW;
        }

        /* Share the same physical page */
        page_ref_inc(pa);
        if (mappages(dst, va, PAGE_SIZE, pa, flags) < 0) {
            /* BF: Rollback — restore parent PTE and decrement refcount
             * leaked by the failed child mapping attempt. */
            *pte = PA2PTE(pa) | orig_flags;
            page_ref_dec(pa);
            sfence_vma();
            return -1;
        }

        /* BF: B4 (page_add_mapping) intentionally omitted here.
         * copy_pagetable_cow runs for ALL pages (anon + non-anon), but
         * freewalk_level in the exit path does kfree without a matching
         * page_remove_mapping.  This causes mapcount to stay elevated
         * after the child exits, making pages permanently unevictable
         * by swap_evict_pages (which skips mapcount > 0).  The VMA-level
         * rmap from anon_vma_fork is sufficient for swap/reclaim. */
        va += PAGE_SIZE;
    }
    /* Flush TLB since we modified parent PTEs */
    sfence_vma();
    return 0;
}

/* ------------------------------------------------------------------ */
/* cleanup_proc_resources: free or release shared resources on exit      */
/* Called from SYS_exit, signal default-kill, and SYS_kill SIGKILL.     */
/* ------------------------------------------------------------------ */
/* RF: Made non-static for access by extracted process syscall module */
void cleanup_proc_resources(struct proc *p) {
    /* ---- POSIX file locks ---- */
    /* POSIX: all locks held by a process are released on exit.
     * OFD locks are released separately when the open_file refcount hits 0. */
    file_locks_release_pid(p->pid);

    /* BF: Write back MAP_SHARED pages before freeing VMAs on exit.
     * Must run while p->pagetable is still valid because
     * mmap_writeback_shared calls walk() (software PT walk, identity-mapped).
     * Skip for CLONE_VM threads when siblings still share the address
     * space (refcount > 1) to avoid racing with sibling modifications.
     * Only the last thread exiting performs the writeback. */
    if (!p->shared_pt || p->shared_pt->refcount <= 1)
        mmap_writeback_shared(p, 0, MAXVA);

    /* ---- Pagetable / VMAs ---- */
    /* Switch to kernel_pagetable before freeing the process's page table.
     * The kernel runs on the process's page table (shared kernel mapping),
     * so we must switch away before freeing it. */
    if (p->shared_pt) {
        p->shared_pt->refcount--;
        if (p->shared_pt->refcount == 0) {
            pagetable_t old_pt = p->pagetable;
            p->pagetable = kernel_pagetable;
            w_satp(MAKE_SATP(kernel_pagetable));
            sfence_vma();
            vma_free_all(p);
            proc_freepagetable(old_pt);
            kmfree(p->shared_pt, sizeof(struct shared_pagetable));
        } else {
            vma_free_all(p);
        }
        p->shared_pt = NULL;
        if (p->pagetable != kernel_pagetable)
            p->pagetable = kernel_pagetable;
    } else {
        pagetable_t old_pt = p->pagetable;
        p->pagetable = kernel_pagetable;
        w_satp(MAKE_SATP(kernel_pagetable));
        sfence_vma();
        vma_free_all(p);
        proc_freepagetable(old_pt);
    }

    /* ---- File descriptors ---- */
    if (p->shared_ft) {
        p->shared_ft->refcount--;
        if (p->shared_ft->refcount == 0) {
            /* Last user: close all fds in the shared table.
             * close_ofile uses proc_ofile() which returns shared_ft->ofile. */
            for (int i = 0; i < NOFILE; i++) {
                if (p->shared_ft->ofile[i])
                    close_ofile(p, i);
            }
            kmfree(p->shared_ft, sizeof(struct shared_ftable));
        }
        p->shared_ft = NULL;
    } else {
        /* Traditional: close fds in proc's own ofile[] */
        for (int i = 0; i < NOFILE; i++) {
            if (p->ofile[i])
                close_ofile(p, i);
        }
    }
    /* ENH: Clear per-fd flags on process exit */
    memset(p->fd_flags, 0, sizeof(p->fd_flags));

    /* ---- Signal actions ---- */
    if (p->shared_sa) {
        p->shared_sa->refcount--;
        if (p->shared_sa->refcount == 0)
            kmfree(p->shared_sa, sizeof(struct shared_sigactions));
        p->shared_sa = NULL;
    }
    /* Inline sigactions need no freeing */
}

/* ------------------------------------------------------------------ */
/* sys_clone: fork or create a thread in the current process            */
/* a0 = flags (CLONE_VM | CLONE_FILES | CLONE_SIGHAND | SIGCHLD etc.)  */
/* a1 = child stack pointer (0 means inherit parent sp)                 */
/* ------------------------------------------------------------------ */
/* RF: Made static -- only called from sys_clone_handler in this module */
static pid_t sys_clone(struct trapframe *caller_tf) {
    uint64_t flags    = caller_tf->a0;
    uint64_t child_sp = caller_tf->a1;
    int clone_flags   = (int)(flags & ~0xFFUL); /* mask out low-byte signal */
    pagetable_t new_pt = 0; /* track COW child PT for cleanup on failure */

    /* CLONE_SIGHAND requires CLONE_VM (Linux semantics) */
    if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
        return -22; /* -EINVAL */

    struct proc *p = allocproc();
    if (!p) return -1;

    /* ---- Common path (all modes) ---- */
    memcpy(p->cwd, current_proc->cwd, sizeof(p->cwd));
    p->umask_val = current_proc->umask_val;
    memcpy(p->name, current_proc->name, sizeof(p->name));

    /* Copy trapframe — child starts after the ecall just like parent */
    memcpy(p->trapframe, caller_tf, sizeof(struct trapframe));
    p->trapframe->a0 = 0; /* child returns 0 */

    /* Set child stack pointer if caller provided one */
    if (child_sp != 0)
        p->trapframe->sp = child_sp;

    p->parent = current_proc;
    p->pgid = current_proc->pgid;
    p->sid = current_proc->sid;
    /* BF: Propagate UID/GID to child so kill() permission checks
     * remain consistent after fork. */
    p->uid = current_proc->uid;
    p->gid = current_proc->gid;
    p->clone_flags = clone_flags;

    /* BF: POSIX requires fork to inherit the parent's alternate signal
     * stack settings.  Without this, allocproc's default SS_DISABLE
     * overwrites the parent's configured alt stack, causing sigaltstack
     * GET in the child to report SS_DISABLE instead of the inherited
     * flags. */
    p->ss_sp = current_proc->ss_sp;
    p->ss_size = current_proc->ss_size;
    p->ss_flags = current_proc->ss_flags;

    /* Copy stack bounds and mmap_base from parent */
    p->stack_top = current_proc->stack_top;
    p->stack_bottom = current_proc->stack_bottom;
    p->mmap_base = current_proc->mmap_base;
    p->sz = current_proc->sz;

    /* ---- CLONE_VM: share address space ---- */
    if (clone_flags & CLONE_VM) {
        /* Promote parent's pagetable to shared if not already */
        if (!current_proc->shared_pt) {
            struct shared_pagetable *spt =
                (struct shared_pagetable *)kmalloc(sizeof(struct shared_pagetable));
            if (!spt) goto bad;
            spt->pt = current_proc->pagetable;
            spt->refcount = 1;
            current_proc->shared_pt = spt;
            current_proc->clone_flags |= CLONE_VM;
        }
        current_proc->shared_pt->refcount++;
        p->shared_pt = current_proc->shared_pt;
        p->pagetable = p->shared_pt->pt;

        /* Deep copy VMA list (Option A from design doc) */
        vma_dup(current_proc, p);
    } else {
        /* Traditional COW fork: build independent child page table.
         * NO kernel identity mappings — the trampoline switches satp
         * to kernel_pagetable on trap entry. */
        new_pt = uvmcreate();
        if (!new_pt) goto bad;

        /* COW-share user pages first (creates level-1 table for VPN[2]=0) */
        if (copy_pagetable_cow(new_pt, current_proc->pagetable,
                               0, MAXVA) < 0)
            goto bad;

        /* Map kernel identity pages into child page table (must be after
         * copy_pagetable_cow so pt[0] exists for MMIO level-1 sharing) */
        if (uvmmap_kernel(new_pt) < 0) goto bad;

        p->pagetable = new_pt;
        new_pt = 0; /* success — ownership transferred to p */

        /* Deep copy VMA list */
        vma_dup(current_proc, p);
    }

    /* ---- CLONE_FILES: share file descriptor table ---- */
    if (clone_flags & CLONE_FILES) {
        /* Promote parent's ofile[] to shared if not already */
        if (!current_proc->shared_ft) {
            struct shared_ftable *sft =
                (struct shared_ftable *)kmalloc(sizeof(struct shared_ftable));
            if (!sft) goto bad;
            for (int i = 0; i < NOFILE; i++)
                sft->ofile[i] = current_proc->ofile[i];
            sft->refcount = 1;
            current_proc->shared_ft = sft;
            current_proc->clone_flags |= CLONE_FILES;
        }
        current_proc->shared_ft->refcount++;
        p->shared_ft = current_proc->shared_ft;
    } else {
        /* Traditional: copy file descriptors independently */
        copy_ofile(p, current_proc);
    }

    /* ---- CLONE_SIGHAND: share signal handlers ---- */
    if (clone_flags & CLONE_SIGHAND) {
        /* Promote parent's sigactions to shared if not already */
        if (!current_proc->shared_sa) {
            struct shared_sigactions *ssa =
                (struct shared_sigactions *)kmalloc(sizeof(struct shared_sigactions));
            if (!ssa) goto bad;
            memcpy(ssa->actions, current_proc->sigactions,
                   sizeof(ssa->actions));
            ssa->refcount = 1;
            current_proc->shared_sa = ssa;
            current_proc->clone_flags |= CLONE_SIGHAND;
        }
        current_proc->shared_sa->refcount++;
        p->shared_sa = current_proc->shared_sa;
    } else {
        /* Traditional: copy signal actions by value */
        memcpy(p->sigactions, current_proc->sigactions,
               sizeof(p->sigactions));
    }

    /* Child's kernel context: resume at forkret */
    p->context.ra = (uint64_t)forkret;
    /* BF: Fork child inherits parent's kstack_npages for proper stack top */
    p->context.sp = (uint64_t)p->kstack + (uint64_t)p->kstack_npages * PAGE_SIZE;

    p->state = RUNNABLE;
    return p->pid;

bad:
    /* Clean up partially-built child pagetable.  proc_freepagetable
     * walks the user address space, freeing COW-shared pages (via
     * kfree/refcount) AND any duplicated swap slots, preventing both
     * memory leaks and swap-slot double-frees. */
    if (new_pt)
        proc_freepagetable(new_pt);
    p->state = UNUSED;
    return -1;
}

/* ------------------------------------------------------------------ */
/* sys_wait4: wait for a child to exit                                  */
/* ------------------------------------------------------------------ */
/* RF: Made static -- only called from sys_wait4_handler in this module */
static pid_t sys_wait4(pid_t wpid, int *wstatus, int options) {
    for (;;) {
        int found_child = 0;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->state == UNUSED) continue;
            if (p->parent != current_proc) continue;
            /* wpid >  0: wait for specific child */
            /* wpid == 0: wait for children in caller's process group */
            /* wpid < -1: wait for children in process group -wpid */
            /* wpid == -1: wait for any child */
            if (wpid > 0 && p->pid != wpid) continue;
            if (wpid == 0 && p->pgid != current_proc->pgid) continue;
            if (wpid < -1 && p->pgid != (pid_t)(-wpid)) continue;
            found_child = 1;
            if (p->state == ZOMBIE) {
                pid_t pid = p->pid;
                if (wstatus) {
                    if (p->killed_by_signal) {
                        /* Signal kill: low 7 bits = signal number */
                        *wstatus = p->killed_by_signal & 0x7f;
                    } else {
                        /* Normal exit: exit code in upper byte, low byte = 0 */
                        *wstatus = (p->exit_status & 0xff) << 8;
                    }
                }
                /* free child resources */
                /* BF: Free all pages of the kernel stack (1 or 2 pages) */
                if (p->kstack) {
                    kfree_contiguous(p->kstack, p->kstack_npages);
                    p->kstack = NULL;
                }
                if (p->trapframe) { kfree(p->trapframe); p->trapframe = NULL; }
                p->state = UNUSED;
                p->pid = 0;
                p->parent = NULL;
                /* BF: Clear in_copy_fault so the slot can be reused cleanly
                 * by allocproc without inheriting stale recursion guard state. */
                p->in_copy_fault = 0;
                return pid;
            }
            /* WUNTRACED: report stopped children */
            if ((options & 2) && p->stopped_signal) {
                pid_t pid = p->pid;
                if (wstatus)
                    *wstatus = (p->stopped_signal << 8) | 0x7f;
                /* Clear stopped_signal so we don't report it again */
                p->stopped_signal = 0;
                return pid;
            }
        }
        if (!found_child) return -10; /* -ECHILD */
        /* WNOHANG: don't block */
        if (options & 1) return 0;
        /* Otherwise sleep until a child exits */
        if (current_proc) {
            current_proc->waiting_for_uart = 0;
            /* BF: Mark this process as a wait4 sleeper so setpgid can
             * selectively wake only waitpid callers, not pipe/poll/futex. */
            current_proc->waiting_for_child = 1;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
            /* BF: Clear wait4 sleeper flag after waking — process is no
             * longer blocked in wait4. */
            current_proc->waiting_for_child = 0;
            /* Check for pending signals after waking up:
             * - User handler (handler > 1): return -EINTR
             * - SIG_DFL terminating signal: return -EINTR to let deliver_signal kill us
             * - SIG_IGN / SIG_DFL ignore-class: continue waiting
             * BF: SIGCHLD with user handler must NOT return EINTR — it's the
             * signal that woke us to re-check children. Returning EINTR would
             * prevent wait4 from finding the zombie (or detecting ECHILD when
             * SA_NOCLDWAIT auto-reaped the child). Let the loop re-check. */
            {
                uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                struct kern_sigaction *ksa = proc_sigactions(current_proc);
                for (int s = 1; s < 32 && deliverable; s++) {
                    if (deliverable & signal_bit(s)) {
                        if (ksa[s].handler > 1) { /* user handler */
                            /* BF: SIGCHLD doesn't interrupt wait4 — it's the
                             * reason we woke up. Let the loop re-scan children.
                             * The signal will be delivered after wait4 returns. */
                            if (s == SIGCHLD) continue;
                            return -4; /* -EINTR */
                        }
                        if (ksa[s].handler == 0) { /* SIG_DFL */
                            /* Ignore-class: SIGCHLD, SIGURG, SIGWINCH (continue waiting) */
                            if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                                continue;
                            /* Stop-class: handled by deliver_signal, don't terminate */
                            if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                                continue;
                            /* SIGCONT: resume, not terminate */
                            if (s == SIGCONT)
                                continue;
                            /* All other SIG_DFL signals terminate; return so
                             * deliver_signal can kill the process */
                            return -4; /* -EINTR */
                        }
                        /* SIG_IGN (handler == 1): continue waiting */
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Syscall handlers                                                     */
/* ------------------------------------------------------------------ */

long sys_clone_handler(struct trapframe *tf) {
    pid_t child = sys_clone(tf);
    tf->a0 = (uint64_t)child;
    return SYSCALL_NORETURN;
}

long sys_execve_handler(struct trapframe *tf) {
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a0, sizeof(kpath)) < 0) {
        return (long)-14;
    }

    uint64_t uargv = tf->a1;
    uint64_t uenvp = tf->a2;

    #define EXECVE_MAXARGS  32
    #define EXECVE_MAXENVS  32

    char *kstrbuf = (char *)kalloc();
    if (!kstrbuf) {
        return (long)-12; /* -ENOMEM */
    }

    int kargc = 0;
    int kstroff = 0;
    int kstroff_arr[EXECVE_MAXARGS];

    if (uargv) {
        while (kargc < EXECVE_MAXARGS) {
            uint64_t uptr;
            if (copyin(current_proc->pagetable, &uptr, uargv + (uint64_t)kargc * 8, 8) < 0)
                break;
            if (uptr == 0) break;
            /* BF: Copy argv strings directly into kstrbuf instead of a 128-byte
             * stack tmpstr.  The old code silently dropped all remaining args
             * when any single string exceeded 127 chars (copyinstr returned -1
             * and the loop broke).  Now strings up to PAGE_SIZE-kstroff bytes
             * are supported, and we distinguish EFAULT (bad pointer) from E2BIG
             * (string too long for the remaining page space). */
            int remaining = (int)PAGE_SIZE - kstroff;
            if (remaining <= 0) break; /* page full */
            {
                /* Probe 1 byte to distinguish bad pointer from string too long */
                char probe;
                if (copyin(current_proc->pagetable, &probe, uptr, 1) < 0) {
                    /* BF: kenvbuf not yet allocated at this point, only free kstrbuf */
                    kfree(kstrbuf);
                    return (long)-14; /* -EFAULT */
                }
            }
            if (copyinstr(current_proc->pagetable, kstrbuf + kstroff, uptr, remaining) < 0) {
                /* String does not fit in remaining page space — arg list too big */
                /* BF: kenvbuf not yet allocated at this point, only free kstrbuf */
                kfree(kstrbuf);
                return (long)-7; /* -E2BIG */
            }
            int slen = strlen(kstrbuf + kstroff) + 1;
            kstroff_arr[kargc] = kstroff;
            kstroff += slen;
            kargc++;
        }
    }

    char *kenvbuf = (char *)kalloc();
    int kenvc = 0;
    int kenvoff = 0;
    int kenvoff_arr[EXECVE_MAXENVS];

    if (kenvbuf && uenvp) {
        while (kenvc < EXECVE_MAXENVS) {
            uint64_t uptr;
            if (copyin(current_proc->pagetable, &uptr, uenvp + (uint64_t)kenvc * 8, 8) < 0)
                break;
            if (uptr == 0) break;
            /* BF: Copy envp strings directly into kenvbuf instead of a 128-byte
             * stack tmpstr.  Env vars like PATH commonly exceed 128 chars; the
             * old code silently dropped them and all subsequent vars.  Now
             * strings up to PAGE_SIZE-kenvoff bytes are supported, and we
             * return proper error codes for bad pointers vs. oversized env. */
            int remaining = (int)PAGE_SIZE - kenvoff;
            if (remaining <= 0) break; /* page full */
            {
                /* Probe 1 byte to distinguish bad pointer from string too long */
                char probe;
                if (copyin(current_proc->pagetable, &probe, uptr, 1) < 0) {
                    kfree(kstrbuf);
                    kfree(kenvbuf);
                    return (long)-14; /* -EFAULT */
                }
            }
            if (copyinstr(current_proc->pagetable, kenvbuf + kenvoff, uptr, remaining) < 0) {
                /* String does not fit in remaining page space — env list too big */
                kfree(kstrbuf);
                kfree(kenvbuf);
                return (long)-7; /* -E2BIG */
            }
            int slen = strlen(kenvbuf + kenvoff) + 1;
            kenvoff_arr[kenvc] = kenvoff;
            kenvoff += slen;
            kenvc++;
        }
    }

    char resolved[128];
    char tarpath[128];
    /* BF: Propagate ENAMETOOLONG from resolve_path */
    if (resolve_path(current_proc->cwd, kpath, resolved) < 0) {
        kfree(kstrbuf);
        if (kenvbuf) kfree(kenvbuf);
        return (long)-36; /* -ENAMETOOLONG */
    }
    /* BF: Reduced maxdepth from 8 to 6 to prevent kernel stack overflow.
     * Each recursion level costs ~1152 bytes; 6*1152+600 = 7512, leaving
     * ~680 bytes margin on the 8KB kernel stack. */
    /* BF: Propagate all negative errors from resolve_symlinks_in_path */
    { int sr = resolve_symlinks_in_path(resolved, 6, 1);
      if (sr < 0) { kfree(kstrbuf); if (kenvbuf) kfree(kenvbuf); return (long)sr; } }
    /* BF: Pass sizeof(tarpath) to prevent buffer overflow; on ENAMETOOLONG
     * skip the tarfs lookup — the binary may still exist on ext2/VFS. */
    int ret = -2; /* default to ENOENT if tarfs path conversion fails */
    if (to_tarfs_path(resolved, tarpath, sizeof(tarpath)) == 0)
        ret = exec(tarpath);
    if (ret < 0) {
        char orig_tarpath[128];
        /* BF: Skip tarfs fallback on ENAMETOOLONG — binary may be on ext2 */
        if (to_tarfs_path(kpath, orig_tarpath, sizeof(orig_tarpath)) == 0)
            ret = exec(orig_tarpath);
    }
    if (ret < 0) {
        ret = exec(kpath);
    }
    /* Try loading ELF from VFS (ext2 or other mounted filesystem) */
    if (ret < 0) {
        ret = exec_vfs(resolved);
    }

    /* Shebang (#!) handling: if exec failed, check if file is a script */
    int shebang_file_found = 0;
    if (ret < 0) {
        char first_bytes[128];
        int got = 0;
        struct posix_header *th = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
        if (!th) {
            char orig_tp[128];
            /* BF: Skip tarfs fallback on ENAMETOOLONG — file may be on ext2 */
            if (to_tarfs_path(kpath, orig_tp, sizeof(orig_tp)) == 0)
                th = tar_find_file(_tarfs_start, _tarfs_end, orig_tp);
        }
        if (th) {
            char *data = (char *)th + sizeof(struct posix_header);
            for (int j = 0; j < 255 && data[j] != '\n' && data[j] != '\0'; j++)
                first_bytes[got++] = data[j];
            first_bytes[got] = '\0';
        }
        /* VFS fallback: read first line from ext2 or other mounted fs */
        if (got == 0) {
            const char *vfs_relpath;
            struct mount *vmnt = vfs_lookup_mount_entry(resolved, &vfs_relpath);
            struct vfs_ops *vops = vmnt ? vmnt->ops : NULL;
            if (vops && vops->open) {
                void *vpriv = NULL;
                if (vops->open(vmnt->fs_data, vfs_relpath, 0, &vpriv) >= 0) {
                    /* File exists on VFS — mark as found even if read fails */
                    shebang_file_found = 1;
                    int64_t voff = 0;
                    if (vops->read) {
                        int64_t nr = vops->read(vmnt->fs_data, vpriv,
                                                first_bytes, 255, &voff);
                        if (nr > 0) {
                            for (int j = 0; j < (int)nr; j++) {
                                if (first_bytes[j] == '\n' || first_bytes[j] == '\0') {
                                    got = j;
                                    break;
                                }
                            }
                            if (got == 0 && nr > 0) got = (nr > 255) ? 255 : (int)nr;
                        }
                    }
                    first_bytes[got] = '\0';
                    if (vops->close) vops->close(vmnt->fs_data, vpriv);
                }
            }
        }
        if (got > 0) shebang_file_found = 1;
        if (got >= 2 && first_bytes[0] == '#' && first_bytes[1] == '!') {
            /* Parse interpreter path */
            char interp[128];
            int j = 2;
            while (first_bytes[j] == ' ' || first_bytes[j] == '\t') j++;
            int k = 0;
            while (j < got && first_bytes[j] != '\n' && first_bytes[j] != '\r' &&
                   first_bytes[j] != ' ' && first_bytes[j] != '\t' && k < 254)
                interp[k++] = first_bytes[j++];
            interp[k] = '\0';

            if (k > 0) {
                /* Rebuild argv: [interp, script_path, original_args[1:]...] */
                int interp_len = strlen(interp) + 1;
                int script_len = strlen(resolved) + 1;

                /* Rebuild argv: [interp, script_path, old_argv[1:]...] */
                int new_kstroff = 0;
                char *tmpbuf = (char *)kalloc();
                if (tmpbuf) {
                    /* Copy interp */
                    memcpy(tmpbuf, interp, interp_len);
                    int new_offsets[EXECVE_MAXARGS];
                    int new_argc = 0;
                    new_offsets[new_argc++] = 0;
                    new_kstroff = interp_len;

                    /* Copy script path */
                    new_offsets[new_argc++] = new_kstroff;
                    memcpy(tmpbuf + new_kstroff, resolved, script_len);
                    new_kstroff += script_len;

                    /* Copy old_argv[1..] */
                    for (int a = 1; a < kargc && new_argc < EXECVE_MAXARGS; a++) {
                        int slen = strlen(kstrbuf + kstroff_arr[a]) + 1;
                        if (new_kstroff + slen > (int)PAGE_SIZE) break;
                        new_offsets[new_argc] = new_kstroff;
                        memcpy(tmpbuf + new_kstroff, kstrbuf + kstroff_arr[a], slen);
                        new_kstroff += slen;
                        new_argc++;
                    }

                    /* Replace kstrbuf with new argv */
                    memcpy(kstrbuf, tmpbuf, new_kstroff);
                    kargc = new_argc;
                    kstroff = new_kstroff;
                    for (int a = 0; a < kargc; a++)
                        kstroff_arr[a] = new_offsets[a];
                    kfree(tmpbuf);

                    /* Now exec the interpreter (follow symlinks) */
                    char interp_resolved[128];
                    /* BF: Propagate ENAMETOOLONG from resolve_path */
                    if (resolve_path("/", interp, interp_resolved) < 0) {
                        ret = -36; /* -ENAMETOOLONG */
                    } else {
                        /* BF: Propagate all negative errors from symlink resolution */
                        int sr = resolve_symlinks_in_path(interp_resolved, 6, 1);
                        if (sr < 0) {
                            ret = sr;
                        } else {
                            char interp_tarpath[128];
                            /* BF: Skip tarfs lookup on ENAMETOOLONG — interpreter
                             * may still be loadable from ext2/VFS path. */
                            if (to_tarfs_path(interp_resolved, interp_tarpath,
                                              sizeof(interp_tarpath)) == 0)
                                ret = exec(interp_tarpath);
                        }
                    }
                }
            }
        }
    }

    if (ret < 0) {
        kfree(kstrbuf);
        if (kenvbuf) kfree(kenvbuf);
        /* Return ENOEXEC if file was found but not executable, else ENOENT */
        return (long)(shebang_file_found ? -8 : -2);
    }

    /* POSIX: close file descriptors marked with FD_CLOEXEC.
     * ENH: Check per-fd fd_flags (bit 0 = FD_CLOEXEC) in addition to the
     * legacy O_CLOEXEC check on the open file flags, so that both old and
     * new paths are covered. */
    {
        #define KERN_O_CLOEXEC 02000000 /* matches libc O_CLOEXEC */
        struct open_file **oft = proc_ofile(current_proc);
        for (int fd = 0; fd < NOFILE; fd++) {
            if (oft[fd] && ((oft[fd]->flags & KERN_O_CLOEXEC) ||
                            (current_proc->fd_flags[fd] & 1))) {
                close_ofile(current_proc, fd);
                current_proc->fd_flags[fd] = 0;
            }
        }
    }

    /* BF: POSIX requires caught signal dispositions (user handlers) to be
     * reset to SIG_DFL on exec.  SIG_IGN dispositions are preserved.
     * Also reset the alternate signal stack to SS_DISABLE since the old
     * stack memory is gone after exec. */
    {
        struct kern_sigaction *sa = proc_sigactions(current_proc);
        for (int i = 1; i < 32; i++) {
            if (sa[i].handler > 1) { /* user handler, not SIG_DFL(0) or SIG_IGN(1) */
                sa[i].handler = 0;  /* reset to SIG_DFL */
                sa[i].flags = 0;
                sa[i].mask = 0;
            }
        }
    }
    current_proc->ss_sp = 0;
    current_proc->ss_size = 0;
    current_proc->ss_flags = 2; /* SS_DISABLE */

    /* Mark that this process has exec'd (for POSIX setpgid restrictions) */
    current_proc->has_execed = 1;

    {
        struct trapframe *newtf = current_proc->trapframe;

        int argv_ptrs_sz = (kargc + 1) * 8;
        int envp_ptrs_sz = (kenvc + 1) * 8;
        int header       = 8;
        int total        = header + argv_ptrs_sz + envp_ptrs_sz + kstroff + kenvoff;
        total = (total + 15) & ~15;

        uint64_t sp            = USER_STACK_TOP - 32 - total;
        uint64_t argv_ptr_base = sp + header;
        uint64_t envp_ptr_base = sp + header + argv_ptrs_sz;
        uint64_t argv_str_base = sp + header + argv_ptrs_sz + envp_ptrs_sz;
        uint64_t envp_str_base = argv_str_base + kstroff;

        /* BF: Use multi-chunk copyout to avoid kbuf overflow.
         * The old code copied header + pointer arrays + all strings into a
         * single 4096-byte kalloc page, but total could reach ~8KB when
         * argv and envp strings fill their respective pages.  Now we copy
         * only the header + pointer arrays into kbuf (max ~536 bytes),
         * then copyout argv and envp strings directly from their own
         * already-allocated pages. */
        char *kbuf = (char *)kalloc();
        if (kbuf) {
            int off = 0;
            uint64_t u_argc = (uint64_t)kargc;
            memcpy(kbuf + off, &u_argc, 8); off += 8;

            uint64_t ptrs[EXECVE_MAXARGS > EXECVE_MAXENVS ?
                          EXECVE_MAXARGS + 1 : EXECVE_MAXENVS + 1];
            int str_off2 = 0;
            for (int i = 0; i < kargc; i++) {
                ptrs[i] = argv_str_base + str_off2;
                int slen = strlen(kstrbuf + kstroff_arr[i]) + 1;
                str_off2 += slen;
            }
            ptrs[kargc] = 0;
            memcpy(kbuf + off, ptrs, argv_ptrs_sz);
            off += argv_ptrs_sz;

            str_off2 = 0;
            for (int i = 0; i < kenvc; i++) {
                ptrs[i] = envp_str_base + str_off2;
                int slen = strlen(kenvbuf + kenvoff_arr[i]) + 1;
                str_off2 += slen;
            }
            ptrs[kenvc] = 0;
            memcpy(kbuf + off, ptrs, envp_ptrs_sz);
            off += envp_ptrs_sz;

            /* Multi-chunk copyout: header+ptrs first, then string pages */
            copyout(current_proc->pagetable, sp, kbuf, off);
            if (kstroff > 0)
                copyout(current_proc->pagetable, argv_str_base, kstrbuf, kstroff);
            if (kenvoff > 0)
                copyout(current_proc->pagetable, envp_str_base, kenvbuf, kenvoff);
            kfree(kbuf);

            newtf->sp = sp;
            newtf->a0 = (uint64_t)kargc;
            newtf->a1 = argv_ptr_base;
            newtf->a2 = envp_ptr_base;
        } else {
            /* BF: Fallback on kbuf alloc failure -- set argc=0 so the
             * process can at least start rather than reading garbage.
             * sp/a1/a2 are left at exec defaults (entry point sets them). */
            newtf->a0 = 0;
        }
    }

    kfree(kstrbuf);
    if (kenvbuf) kfree(kenvbuf);

    /* BF: Guard against overlapping src/dst (undefined behavior for memcpy).
     * When tf already points to current_proc->trapframe, no copy is needed. */
    if (tf != current_proc->trapframe)
        memcpy(tf, current_proc->trapframe, sizeof(struct trapframe));
    return SYSCALL_NORETURN;
}

long sys_fexecve_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t uargv = tf->a1;
    uint64_t uenvp = tf->a2;

    /* Validate fd */
    if (fd < 0 || fd >= NOFILE)
        return (long)-9; /* EBADF */

    struct open_file **oft = proc_ofile(current_proc);
    struct open_file *of = oft[fd];
    if (!of)
        return (long)-9; /* EBADF */

    /* Get the path from the open file descriptor */
    if (of->path[0] == '\0')
        return (long)-8; /* ENOEXEC - no path available */

    /* Build a temporary path copy */
    char path_copy[256];
    size_t path_len = strlen(of->path);
    if (path_len >= sizeof(path_copy))
        path_len = sizeof(path_copy) - 1;
    memcpy(path_copy, of->path, path_len);
    path_copy[path_len] = '\0';

    /* Allocate kernel page for path string */
    char *kpath_page = (char *)kalloc();
    if (!kpath_page)
        return (long)-12; /* ENOMEM */

    memcpy(kpath_page, path_copy, path_len + 1);

    /* Temporarily map kernel path to user space */
    uint64_t user_path_addr = USER_STACK_TOP - PAGE_SIZE * 2; /* temporary location */

    /* Copy path to user space */
    if (copyout(current_proc->pagetable, user_path_addr, kpath_page, path_len + 1) < 0) {
        kfree(kpath_page);
        return (long)-14; /* EFAULT */
    }

    kfree(kpath_page);

    /* Now call execve with the path from fd */
    tf->a0 = user_path_addr;
    tf->a1 = uargv;
    tf->a2 = uenvp;

    /* Call the execve handler */
    return sys_execve_handler(tf);
}

long sys_wait4_handler(struct trapframe *tf) {
    pid_t wpid   = (pid_t)(int)tf->a0;
    int options  = (int)tf->a2;
    int kwstatus = 0;
    pid_t ret = sys_wait4(wpid, tf->a1 ? &kwstatus : NULL, options);
    if (ret > 0 && tf->a1) {
        if (copyout(current_proc->pagetable, tf->a1, &kwstatus, sizeof(kwstatus)) < 0) {
            return (long)-14;
        }
    }
    return (long)ret;
}

long sys_getpid_handler(struct trapframe *tf) {
    (void)tf;
    return current_proc ? (long)current_proc->pid : 0;
}

long sys_getppid_handler(struct trapframe *tf) {
    (void)tf;
    if (current_proc && current_proc->parent)
        return (long)current_proc->parent->pid;
    else
        return (long)1;
}

long sys_getuid_handler(struct trapframe *tf) {
    (void)tf;
    /* BF: Return actual process UID instead of hardcoded 0 so that
     * userspace identity checks (e.g. kill permission) are accurate. */
    return (long)current_proc->uid;
}

long sys_getgid_handler(struct trapframe *tf) {
    (void)tf;
    /* BF: Return actual process GID instead of hardcoded 0. */
    return (long)current_proc->gid;
}

long sys_exit_handler(struct trapframe *tf) {
    int status = (int)tf->a0;
    if (current_proc) {
        current_proc->exit_status = status;
        current_proc->killed_by_signal = 0;
        current_proc->stopped_signal = 0;
        cleanup_proc_resources(current_proc);
        if (current_proc->pid == 1) {
            ext2_sync(NULL); /* flush filesystems before halt */
            printk("\nSystem halted (init exited with status %d).\n", status);
            printk("Process stats: highwater=%d exhaustions=%d\n",
                   get_proc_highwater(), get_proc_exhaustion_count());
            register unsigned long a0 asm("a0") = 0;
            register unsigned long a1 asm("a1") = 0;
            register unsigned long a7 asm("a7") = 0x53525354;
            asm volatile("ecall" : : "r"(a0), "r"(a1), "r"(a7));
            while (1) {}
        }
        if (current_proc->parent) {
            struct proc *par = current_proc->parent;
            struct kern_sigaction *psa = proc_sigactions(par);
            current_proc->state = ZOMBIE;
            /* BF: Capture child metadata for parent's SIGCHLD SA_SIGINFO handler.
             * CLD_EXITED for normal exit, CLD_KILLED if terminated by signal. */
            {
                int32_t code = current_proc->killed_by_signal ? K_CLD_KILLED
                                                              : K_CLD_EXITED;
                sig_set_pending_ex(par, SIGCHLD,
                                   (int32_t)current_proc->pid, 0, code,
                                   current_proc->exit_status & 0xff);
            }
            if (par->state == SLEEPING)
                par->state = RUNNABLE;

            /* SA_NOCLDWAIT: auto-reap child instead of leaving zombie */
            if (psa[SIGCHLD].flags & 0x00000002) { /* SA_NOCLDWAIT */
                /* Reparent to init so init reaps it safely via sys_wait4 */
                /* Do NOT kfree(kstack) here, as current thread is still running on it! */
                struct proc *initproc = NULL;
                for (struct proc *p = proc; p < &proc[NPROC]; p++) {
                    if (p->pid == 1 && p->state != UNUSED) { initproc = p; break; }
                }
                current_proc->parent = initproc; /* Can be NULL if init is dead */
                if (initproc && initproc->state == SLEEPING)
                    initproc->state = RUNNABLE;
            }
        } else {
            current_proc->state = ZOMBIE;
        }
        /* Reparent orphan children to init (pid 1) */
        struct proc *initproc = NULL;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->pid == 1 && p->state != UNUSED) { initproc = p; break; }
        }
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->parent == current_proc && p != current_proc) {
                p->parent = initproc;
                /* Wake init if it's sleeping in wait4 */
                if (initproc && initproc->state == SLEEPING)
                    initproc->state = RUNNABLE;
            }
        }

        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->state == SLEEPING && p != current_proc &&
                p->waiting_for_uart == 0)
                p->state = RUNNABLE;
        }
        swtch(&current_proc->context, &scheduler_context);
    }
    while(1) {}
    return SYSCALL_NORETURN;
}

long sys_ps_handler(struct trapframe *tf) {
    int      maxn     = (int)tf->a1;
    int      written  = 0;
    if (tf->a0 && maxn > 0) {
        uint8_t kps[64 * 24];
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->state == UNUSED) continue;
            if (written >= maxn) break;
            char st;
            switch (p->state) {
                case SLEEPING:  st = 'S'; break;
                case RUNNABLE:  st = 'R'; break;
                case RUNNING:   st = 'R'; break;
                case ZOMBIE:    st = 'Z'; break;
                default:        st = '?'; break;
            }
            uint8_t *e = kps + written * 24;
            int32_t pid_  = (int32_t)p->pid;
            int32_t ppid_ = (int32_t)(p->parent ? p->parent->pid : 0);
            memset(e, 0, 24);
            memcpy(e + 0, &pid_,  4);
            memcpy(e + 4, &ppid_, 4);
            e[8] = (uint8_t)st;
            int nl = strlen(p->name);
            if (nl > 15) nl = 15;
            memcpy(e + 9, p->name, nl);
            written++;
        }
        if (written > 0) {
            if (copyout(current_proc->pagetable, tf->a0, kps, (uint64_t)written * 24) < 0) {
                return (long)-14;
            }
        }
    }
    return (long)written;
}

long sys_setpgid_handler(struct trapframe *tf) {
    pid_t target = (pid_t)tf->a0;
    pid_t pgid = (pid_t)tf->a1;
    if (target < 0)
        return (long)-22; /* EINVAL */
    if (pgid < 0)
        return (long)-22; /* EINVAL */
    if (target == 0) target = current_proc->pid;
    if (pgid == 0) pgid = target;

    /* Find target process */
    struct proc *tp = NULL;
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->pid == target && p->state != UNUSED) {
            tp = p;
            break;
        }
    }
    if (!tp) return (long)-3; /* ESRCH */

    /* POSIX: pid must refer to self or a child of caller */
    if (tp != current_proc && tp->parent != current_proc)
        return (long)-3; /* ESRCH */

    /* POSIX: cannot change pgid of session leader (pid == sid) */
    if (tp->pid == tp->sid)
        return (long)-1; /* EPERM */

    /* POSIX: child must not have exec'd (unless targeting self) */
    if (tp != current_proc && tp->has_execed)
        return (long)-13; /* EACCES */

    /* POSIX: target and caller must be in same session */
    if (tp->sid != current_proc->sid)
        return (long)-1; /* EPERM */

    /* Verify pgid target exists in the session (pgid == target is always OK) */
    if (pgid != target) {
        int found = 0;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->state != UNUSED && p->pgid == pgid && p->sid == current_proc->sid) {
                found = 1;
                break;
            }
        }
        if (!found) return (long)-1; /* EPERM: target pgid not in session */
    }

    tp->pgid = pgid;
    /* BF: Wake all wait4 sleepers — they need to re-evaluate match
     * criteria after pgid change.  The previous parent-only wake was
     * too narrow (non-parent waitpid(-pgid) callers missed the wake).
     * A blanket wake was too broad (hit pipe/poll sleepers).  The
     * waiting_for_child flag precisely targets wait4 sleepers. */
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->state == SLEEPING && p->waiting_for_child)
            p->state = RUNNABLE;
    }
    return 0;
}

long sys_setsid_handler(struct trapframe *tf) {
    (void)tf;
    /* POSIX: setsid fails with EPERM if caller is already a process group leader */
    if (current_proc->pid == current_proc->pgid)
        return (long)-1; /* -EPERM */
    current_proc->sid = current_proc->pid;
    current_proc->pgid = current_proc->pid;
    /* BF: Wake all wait4 sleepers — setsid changes the process group,
     * so any waitpid(-pgid) callers need to re-evaluate whether the
     * target group is now empty (should return ECHILD).  Previous code
     * only woke the parent with a waiting_for_uart check, which missed
     * non-parent waiters and used the wrong sleep classification. */
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->state == SLEEPING && p->waiting_for_child)
            p->state = RUNNABLE;
    }
    return (long)current_proc->pid;
}

long sys_getpgid_handler(struct trapframe *tf) {
    pid_t target = (pid_t)tf->a0;
    if (target == 0) {
        return (long)current_proc->pgid;
    } else {
        long result = (long)-3;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->pid == target && p->state != UNUSED) {
                result = (long)p->pgid;
                break;
            }
        }
        return result;
    }
}

long sys_getsid_handler(struct trapframe *tf) {
    pid_t target = (pid_t)tf->a0;
    if (target == 0) {
        return (long)current_proc->sid;
    } else {
        long result = (long)-3;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->pid == target && p->state != UNUSED) {
                result = (long)p->sid;
                break;
            }
        }
        return result;
    }
}
