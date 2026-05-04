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
#include <tar.h>
#include <kmalloc.h>
#include <errno.h>
#include <user_clock.h>

extern char _tarfs_start[];
extern char _tarfs_end[];

/* Minimal Linux-style auxiliary vector entries for external userlands. */
#define AT_NULL    0
#define AT_PAGESZ  6
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_SECURE  23
#define AT_RANDOM  25
#define AT_EXECFN  31
#define EXECVE_AUXV_PAIRS 10

/* ------------------------------------------------------------------ */
/* copy_pagetable_cow: COW-share user pages from src to dst           */
/* ------------------------------------------------------------------ */
/* RF: Made non-static for access by extracted process syscall module */
int copy_pagetable_cow(struct proc *src_proc, pagetable_t dst, pagetable_t src,
                       uint64_t start, uint64_t end) {
    /* Walk a specific VA range and COW-share PTE_U pages.
     * Super page PTEs (level-1 leaf, detected by PTE_IS_LEAF) are skipped
     * by advancing VA to the next 2MB boundary instead of 4KB. */
    for (uint64_t va = start; va < end; ) {
        pte_t l2 = src[PX(2, va)];
        if (!(l2 & PTE_V)) {
            /* BF: USER_TOP spans two Sv39 root entries now.  Fork must not
             * spend a page at a time probing empty 1 GiB holes. */
            va = ((va >> 30) + 1) << 30;
            continue;
        }
        if (PTE_IS_LEAF(l2)) {
            va = ((va >> 30) + 1) << 30;
            continue;
        }
        pagetable_t l1pt = (pagetable_t)PTE2PA(l2);
        pte_t l1 = l1pt[PX(1, va)];
        if (!(l1 & PTE_V)) {
            /* BF: Sparse reservations leave most 2 MiB subtrees absent;
             * skip them so each fork scales with populated page tables. */
            va = SPGROUNDUP(va + 1);
            continue;
        }
        if (PTE_IS_LEAF(l1)) {
            va = SPGROUNDUP(va + 1);
            continue;
        }
        if (va == USER_CLOCK_VA) {
            /* BF: The CPU-clock page is lifecycle-managed outside normal
             * COW so fork gets a fresh clock instead of inheriting CPU time. */
            va += PAGE_SIZE;
            continue;
        }
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
        struct vm_area *vma = src_proc ? vma_find(src_proc, va) : NULL;
        /* BF: Writable MAP_SHARED pages must remain a single writable
         * physical mapping across fork; only private writable pages use COW. */
        int keep_shared_writable = vma &&
                                   (vma->flags & MAP_SHARED) &&
                                   (vma->prot & PROT_WRITE);

        int orig_flags = PTE_FLAGS(*pte);  /* BF: save before CoW conversion for rollback */
        int parent_pte_changed = 0;

        if ((flags & PTE_W) && !keep_shared_writable) {
            /* Writable page: mark read-only + COW in both parent and child */
            if (pte_update(src, va, pte,
                           PA2PTE(pa) |
                           ((flags & ~PTE_W) | PTE_COW | PTE_V),
                           0, PTE_UPDATE_COW, PTE_SFENCE_NONE) < 0)
                return -1;
            flags = (flags & ~PTE_W) | PTE_COW;
            parent_pte_changed = 1;
        }

        /* Share the same physical page */
        page_ref_inc(pa);
        if (mappages(dst, va, PAGE_SIZE, pa, flags) < 0) {
            /* BF: Rollback — restore parent PTE and decrement refcount
             * leaked by the failed child mapping attempt. */
            if (parent_pte_changed) {
                pte_update(src, va, pte, PA2PTE(pa) | orig_flags,
                           0, PTE_UPDATE_COW, PTE_SFENCE_NONE);
            }
            page_ref_dec(pa);
            sfence_vma();
            return -1;
        }

        /* BF: B4 (page_add_mapping) intentionally omitted here.
         * copy_pagetable_cow runs for ALL pages (anon + non-anon), but
         * freewalk_level in the exit path does kfree without a matching
         * page_remove_mapping.  This causes mapcount to stay elevated
         * after the child exits, making pages permanently look shared to
         * later memory-management code.  The VMA-level rmap from
         * anon_vma_fork is sufficient for current teardown paths. */
        va += PAGE_SIZE;
    }
    /* Flush TLB since we modified parent PTEs */
    sfence_vma();
    return 0;
}

/* ------------------------------------------------------------------ */
/* cleanup_proc_resources: free or release shared resources on exit      */
/* Called only while the dying process is current_proc.                  */
/* ------------------------------------------------------------------ */
/* RF: Made non-static for access by extracted process syscall module */
void cleanup_proc_resources(struct proc *p) {
    if (!p || p->resources_released)
        return;
    /* BF: Address-space and fd teardown must run on the dying process's
     * own kernel stack/page-table context.  Cleaning a different process
     * from the sender's syscall path races with that target resuming in a
     * half-destroyed address space, so fail loudly instead. */
    if (p != current_proc) {
        printk("FATAL: cleanup_proc_resources outside current process pid=%d current=%d\n",
               p->pid, current_proc ? current_proc->pid : -1);
        while (1) {}
    }

    /* ---- POSIX file locks ---- */
    /* POSIX: all locks held by a process are released on exit.
     * OFD locks are released separately when the open_file refcount hits 0. */
    file_locks_release_pid(p->pid);

    /* ---- Pagetable / VMAs ---- */
    /* Keep teardown on kernel_pagetable before freeing the process root.
     * User roots are installed only by trap_restore, but this is also safe
     * for older paths that may already be on the kernel root. */
    if (p->shared_pt) {
        pagetable_t old_pt = p->pagetable;
        struct vm_area *old_vmas = p->vma_list;
        p->vma_list = NULL;
        p->shared_pt->refcount--;
        if (p->shared_pt->refcount == 0) {
            p->pagetable = kernel_pagetable;
            w_satp(MAKE_SATP(kernel_pagetable));
            sfence_vma();
            user_clock_unmap(old_pt);
            /* BF: Release PTEs while the saved VMA list is still live so
             * MAP_SHARED writeback and anon rmap cleanup have context. */
            release_user_address_space(old_pt, old_vmas, 1, 1);
            kmfree(p->shared_pt, sizeof(struct shared_pagetable));
        } else {
            /* Shared-vfork child has only a copied VMA list; the parent
             * still owns the shared pagetable mappings. */
            vma_free_chain(old_vmas);
        }
        p->shared_pt = NULL;
        if (p->pagetable != kernel_pagetable)
            p->pagetable = kernel_pagetable;
    } else {
        pagetable_t old_pt = p->pagetable;
        struct vm_area *old_vmas = p->vma_list;
        p->vma_list = NULL;
        p->pagetable = kernel_pagetable;
        w_satp(MAKE_SATP(kernel_pagetable));
        sfence_vma();
        user_clock_unmap(old_pt);
        release_user_address_space(old_pt, old_vmas, 1, 1);
    }

    if (p->user_clock) {
        user_clock_put(p->user_clock);
        p->user_clock = NULL;
    }
    p->cpu_start_ticks = 0;

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
    p->resources_released = 1;
}

/* ------------------------------------------------------------------ */
/* release_vfork_parent: unblock the parent once the vfork child is     */
/* past the shared-mm window via successful exec or any exit path.      */
/* ------------------------------------------------------------------ */
void release_vfork_parent(struct proc *child) {
    if (!(child->clone_flags & CLONE_VFORK))
        return;

    child->clone_flags &= ~CLONE_VFORK;
    if (!child->parent)
        return;

    /* BF: Only clear the specific outstanding vfork wait so unrelated child
     * exits cannot spuriously release a parent that is blocked on another pid. */
    if (child->parent->waiting_for_vfork == child->pid) {
        child->parent->waiting_for_vfork = 0;
        if (child->parent->state == SLEEPING)
            child->parent->state = RUNNABLE;
    }
}

static struct proc *find_initproc(void) {
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->pid == 1 && p->state != UNUSED)
            return p;
    }
    return NULL;
}

static void wake_exit_waiters(struct proc *exiting) {
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->state == SLEEPING && p != exiting &&
            p->waiting_for_uart == 0)
            p->state = RUNNABLE;
    }
}

static void publish_zombie_current(struct proc *p) {
    if (p->parent) {
        struct proc *par = p->parent;
        struct kern_sigaction *psa = proc_sigactions(par);
        int32_t code = p->killed_by_signal ? K_CLD_KILLED : K_CLD_EXITED;
        int32_t status = p->killed_by_signal ? p->killed_by_signal
                                             : (p->exit_status & 0xff);

        p->state = ZOMBIE;
        release_vfork_parent(p);
        sig_set_pending_ex(par, SIGCHLD, (int32_t)p->pid, 0, code, status);
        if (par->state == SLEEPING)
            par->state = RUNNABLE;

        if (psa[SIGCHLD].flags & 0x00000002) { /* SA_NOCLDWAIT */
            struct proc *initproc = find_initproc();
            p->parent = initproc;
            if (initproc && initproc->state == SLEEPING)
                initproc->state = RUNNABLE;
        }
    } else {
        p->state = ZOMBIE;
        release_vfork_parent(p);
    }

    struct proc *initproc = find_initproc();
    for (struct proc *child = proc; child < &proc[NPROC]; child++) {
        if (child->parent == p && child != p) {
            child->parent = initproc;
            if (initproc && initproc->state == SLEEPING)
                initproc->state = RUNNABLE;
        }
    }

    wake_exit_waiters(p);
}

void proc_exit_current(int status, int terminate_signal) {
    struct proc *p = current_proc;
    if (!p)
        while (1) {}

    p->exit_status = status;
    p->killed_by_signal = terminate_signal;
    p->stopped_signal = 0;
    p->pending_terminate = 0;
    p->terminate_signal = 0;

    cleanup_proc_resources(p);

    if (p->pid == 1) {
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

    publish_zombie_current(p);
    swtch(&p->context, &scheduler_context);
    while (1) {}
}

void proc_force_terminate_wake(struct proc *target, int sig) {
    if (!target || target->state == ZOMBIE || target->state == UNUSED)
        return;
    if (!signal_is_valid(sig))
        sig = SIGKILL;

    /* BF: Fatal self-signals must also use deferred teardown.  Exiting
     * synchronously here lets kill(-pgid, SIGKILL) terminate the sender
     * in the middle of the proc-table walk, so later group members never
     * get marked.  Queue the termination request first; the current
     * syscall/trap return path will notice pending_terminate and exit
     * after the full delivery loop completes. */
    target->pending_terminate = 1;
    target->terminate_signal = sig;
    target->exit_status = 128 + sig;
    target->killed_by_signal = sig;
    target->stopped_signal = 0;
    sig_set_pending(target, sig,
                    current_proc ? (int32_t)current_proc->pid : 0,
                    0, K_SI_USER);
    target->sigmask &= ~signal_bit(SIGKILL);
    if (target->state == SLEEPING)
        target->state = RUNNABLE;
}

static void reap_zombie(struct proc *p) {
    /* BF: Reap validates that exit cleanup already released user memory,
     * files, and shared wrappers.  Continuing on failure would hide leaks
     * or double-free bugs by freeing only scheduler-owned resources. */
    if (!p->resources_released) {
        printk("FATAL: reap before cleanup pid=%d state=%d\n",
               p->pid, p->state);
        while (1) {}
    }

    if (p->kstack) {
        kfree_contiguous(p->kstack, p->kstack_npages);
        p->kstack = NULL;
    }
    if (p->trapframe) {
        kfree(p->trapframe);
        p->trapframe = NULL;
    }
    p->state = UNUSED;
    p->pid = 0;
    p->parent = NULL;
    p->exit_status = 0;
    p->killed_by_signal = 0;
    p->stopped_signal = 0;
    p->resources_released = 0;
    p->pending_terminate = 0;
    p->terminate_signal = 0;
    p->in_copy_fault = 0;
    p->user_clock = NULL;
    p->cpu_start_ticks = 0;
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
    int parent_clone_flags_before = current_proc->clone_flags;
    struct shared_pagetable *parent_shared_pt_before = current_proc->shared_pt;
    struct shared_ftable *parent_shared_ft_before = current_proc->shared_ft;
    struct shared_sigactions *parent_shared_sa_before = current_proc->shared_sa;
    int promoted_shared_pt = 0;
    int promoted_shared_ft = 0;
    int promoted_shared_sa = 0;

    /* CLONE_SIGHAND requires CLONE_VM (Linux semantics) */
    if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
        return -22; /* -EINVAL */
    /* BF: Keep CLONE_VM narrowly scoped to Linux-style vfork for now.
     * pdpmake only needs CLONE_VM|CLONE_VFORK|SIGCHLD, and wider shared-mm
     * clone shapes still need stronger thread lifecycle guarantees. */
    if ((clone_flags & CLONE_VFORK) && !(clone_flags & CLONE_VM))
        return -22; /* -EINVAL */
    if ((clone_flags & CLONE_VM) &&
        (clone_flags & ~(CLONE_VM | CLONE_VFORK)))
        return -22; /* -EINVAL */
    if ((clone_flags & CLONE_VM) && !(clone_flags & CLONE_VFORK))
        return -22; /* -EINVAL */

    struct proc *p = allocproc();
    if (!p) return -1;

    /* ---- Common path (all modes) ---- */
    memcpy(p->cwd, current_proc->cwd, sizeof(p->cwd));
    memcpy(p->exe_path, current_proc->exe_path, sizeof(p->exe_path));
    memcpy(p->cmdline, current_proc->cmdline, sizeof(p->cmdline));
    p->cmdline_len = current_proc->cmdline_len;
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
    /* BF: fork inherits the signal mask but not pending signals.  allocproc
     * already clears sigpending, so copy only the active mask here. */
    p->sigmask = current_proc->sigmask;

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
    p->initial_brk = current_proc->initial_brk;

    /* ---- CLONE_VM: share address space ---- */
    if (clone_flags & CLONE_VM) {
        /* Promote parent's pagetable to shared if not already */
        if (!current_proc->shared_pt) {
            struct shared_pagetable *spt =
                (struct shared_pagetable *)kmalloc(sizeof(struct shared_pagetable));
            if (!spt) goto bad;
            spt->pt = current_proc->pagetable;
            spt->refcount = 1;
            spt->clock = current_proc->user_clock;
            current_proc->shared_pt = spt;
            current_proc->clone_flags |= CLONE_VM;
            promoted_shared_pt = 1;
        }
        p->pagetable = current_proc->shared_pt->pt;

        /* BF: CLONE_VM rollback must treat the copied VMA list as a fallible
         * child-only allocation and must not leak the promoted parent wrapper. */
        if (vma_dup(current_proc, p) < 0)
            goto bad;
        current_proc->shared_pt->refcount++;
        p->shared_pt = current_proc->shared_pt;
        /* BF: CLONE_VM/vfork shares one address-space clock mapping.  The
         * wrapper only borrows the pointer; each proc owns its own ref. */
        if (p->user_clock)
            user_clock_put(p->user_clock);
        p->user_clock = current_proc->shared_pt->clock;
        user_clock_get(p->user_clock);
        p->cpu_start_ticks = 0;
    } else {
        /* Traditional COW fork: build independent child page table.
         * Kernel sharing starts at KERNBASE; root entries 0 and 1 remain
         * child-owned so sparse user reservations can fill that range. */
        new_pt = uvmcreate();
        if (!new_pt) goto bad;

        /* COW-share user pages first, then attach the shared high-half kernel
         * roots.  Low roots remain entirely child-owned. */
        if (copy_pagetable_cow(current_proc, new_pt, current_proc->pagetable,
                               0, MAXVA) < 0)
            goto bad;
        /* BF: The user-clock page is a per-process kernel-owned alias.
         * COW fork may encounter stale copies while walking sparse upper
         * roots, so force the child slot empty before installing its clock. */
        user_clock_unmap(new_pt);

        /* Map high-half kernel identity roots into the child page table. */
        if (uvmmap_kernel(new_pt) < 0) goto bad;
        if (user_clock_map(new_pt, p->user_clock) < 0) goto bad;

        /* Deep copy VMA list */
        p->pagetable = new_pt;
        if (vma_dup(current_proc, p) < 0)
            goto bad;
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
            promoted_shared_ft = 1;
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
            promoted_shared_sa = 1;
        }
        current_proc->shared_sa->refcount++;
        p->shared_sa = current_proc->shared_sa;
    } else {
        /* Traditional: copy signal actions by value */
        memcpy(p->sigactions, current_proc->sigactions,
               sizeof(p->sigactions));
    }

    /* Child's kernel context: resume at forkret */
    p->trapframe->user_satp = MAKE_SATP(p->pagetable);
    p->context.ra = (uint64_t)forkret;
    /* BF: Fork child inherits parent's kstack_npages for proper stack top */
    p->context.sp = (uint64_t)p->kstack + (uint64_t)p->kstack_npages * PAGE_SIZE;

    new_pt = 0; /* success — ownership transferred to p */
    p->state = RUNNABLE;

    if (clone_flags & CLONE_VFORK) {
        current_proc->waiting_for_vfork = p->pid;
        /* BF: vfork blocks the parent until the child execs or exits.
         * Re-check after every wake because UART or unrelated child exits can
         * still cause harmless spurious wakeups in the current scheduler. */
        while (current_proc->waiting_for_vfork == p->pid) {
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
        }
    }

    return p->pid;

bad:
    if (p->shared_pt) {
        p->shared_pt->refcount--;
        p->shared_pt = NULL;
    }
    if (p->shared_ft) {
        p->shared_ft->refcount--;
        p->shared_ft = NULL;
    }
    if (p->shared_sa) {
        p->shared_sa->refcount--;
        p->shared_sa = NULL;
    }
    if (promoted_shared_pt && current_proc->shared_pt != parent_shared_pt_before) {
        kmfree(current_proc->shared_pt, sizeof(struct shared_pagetable));
        current_proc->shared_pt = parent_shared_pt_before;
    }
    if (promoted_shared_ft && current_proc->shared_ft != parent_shared_ft_before) {
        kmfree(current_proc->shared_ft, sizeof(struct shared_ftable));
        current_proc->shared_ft = parent_shared_ft_before;
    }
    if (promoted_shared_sa && current_proc->shared_sa != parent_shared_sa_before) {
        kmfree(current_proc->shared_sa, sizeof(struct shared_sigactions));
        current_proc->shared_sa = parent_shared_sa_before;
    }
    current_proc->clone_flags = parent_clone_flags_before;

    /* Clean up partially-built child pagetable.  proc_freepagetable
     * walks the user address space and frees any partially duplicated
     * COW-shared pages through the normal refcounted teardown path. */
    if (new_pt) {
        struct vm_area *failed_vmas = p->vma_list;
        p->vma_list = NULL;
        user_clock_unmap(new_pt);
        release_user_address_space(new_pt, failed_vmas, 0, 1);
    } else {
        vma_free_all(p);
    }
    if (p->user_clock) {
        user_clock_put(p->user_clock);
        p->user_clock = NULL;
    }
    p->cpu_start_ticks = 0;
    if (p->shared_ft) {
        p->shared_ft->refcount--;
        p->shared_ft = NULL;
    } else {
        for (int i = 0; i < NOFILE; i++) {
            if (p->ofile[i])
                close_ofile(p, i);
        }
    }
    if (p->shared_sa) {
        p->shared_sa->refcount--;
        p->shared_sa = NULL;
    }
    if (p->kstack) {
        kfree_contiguous(p->kstack, p->kstack_npages);
        p->kstack = NULL;
    }
    if (p->trapframe) {
        kfree(p->trapframe);
        p->trapframe = NULL;
    }
    p->state = UNUSED;
    return -1;
}

/* ------------------------------------------------------------------ */
/* sys_wait4: wait for a child to exit                                  */
/* ------------------------------------------------------------------ */
/* RF: Made static -- only called from sys_wait4_handler in this module */
static pid_t sys_wait4(pid_t wpid, uint64_t status_uaddr, int options) {
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
                int kwstatus;
                if (p->killed_by_signal) {
                    /* Signal kill: low 7 bits = signal number */
                    kwstatus = p->killed_by_signal & 0x7f;
                } else {
                    /* Normal exit: exit code in upper byte, low byte = 0 */
                    kwstatus = (p->exit_status & 0xff) << 8;
                }
                if (status_uaddr) {
                    if (copyout(current_proc->pagetable, status_uaddr,
                                &kwstatus, sizeof(kwstatus)) < 0)
                        return (pid_t)-EFAULT;
                }
                reap_zombie(p);
                return pid;
            }
            /* WUNTRACED: report stopped children */
            if ((options & 2) && p->stopped_signal) {
                pid_t pid = p->pid;
                int kwstatus = (p->stopped_signal << 8) | 0x7f;
                if (status_uaddr) {
                    if (copyout(current_proc->pagetable, status_uaddr,
                                &kwstatus, sizeof(kwstatus)) < 0)
                        return (pid_t)-EFAULT;
                }
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
                uint64_t deliverable = signal_deliverable_mask(current_proc);
                struct kern_sigaction *ksa = proc_sigactions(current_proc);
                for (int s = 1; s < 65 && deliverable; s++) {
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
    char kpath[KPATH_MAX];
    { int pr = copy_user_path_arg(tf->a0, kpath, sizeof(kpath), 0);
      if (pr < 0) return (long)pr; }

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
                    return (long)-EFAULT; /* -EFAULT */
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
                    return (long)-EFAULT; /* -EFAULT */
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

    char resolved[KPATH_MAX];
    char committed_exe_path[KPATH_MAX];
    char tarpath[128];
    { int rr = resolve_path(current_proc->cwd, kpath, resolved);
      if (rr < 0) {
        kfree(kstrbuf);
        if (kenvbuf) kfree(kenvbuf);
        return (long)rr;
      } }
    /* BF: Keep the resolver depth finite; the resolver keeps large scratch
     * buffers on the heap so long symlink targets do not grow stack frames. */
    /* BF: Propagate all negative errors from resolve_symlinks_in_path */
    { int sr = resolve_symlinks_in_path(resolved, KSYMLINK_MAX_HOPS, 1);
      if (sr < 0) { kfree(kstrbuf); if (kenvbuf) kfree(kenvbuf); return (long)sr; } }
    memcpy(committed_exe_path, resolved, sizeof(committed_exe_path));
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
        char first_bytes[256];
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
            char interp[KPATH_MAX];
            int j = 2;
            while (first_bytes[j] == ' ' || first_bytes[j] == '\t') j++;
            int k = 0;
            while (j < got && first_bytes[j] != '\n' && first_bytes[j] != '\r' &&
                   first_bytes[j] != ' ' && first_bytes[j] != '\t' &&
                   k < KPATH_MAX - 1)
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
                    char interp_resolved[KPATH_MAX];
                    { int rr = resolve_path("/", interp, interp_resolved);
                      if (rr < 0) {
                        ret = rr;
                    } else {
                        /* BF: Propagate all negative errors from symlink resolution */
                        int sr = resolve_symlinks_in_path(interp_resolved, KSYMLINK_MAX_HOPS, 1);
                        if (sr < 0) {
                            ret = sr;
                        } else {
                            char interp_tarpath[128];
                            /* BF: Skip tarfs lookup on ENAMETOOLONG — interpreter
                             * may still be loadable from ext2/VFS path. */
                            if (to_tarfs_path(interp_resolved, interp_tarpath,
                                              sizeof(interp_tarpath)) == 0)
                                ret = exec(interp_tarpath);
                            if (ret >= 0)
                                memcpy(committed_exe_path, interp_resolved,
                                       sizeof(committed_exe_path));
                        }
                    } }
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

    /* BF: exec() loads tarfs paths such as ./bin/sleep internally, but
     * procfs must expose the canonical absolute executable path. */
    memcpy(current_proc->exe_path, committed_exe_path,
           sizeof(current_proc->exe_path));

    /* Snapshot argv image for /proc/<pid>/cmdline (NUL-separated strings). */
    {
        int out = 0;
        if (kargc > 0) {
            for (int i = 0; i < kargc; i++) {
                const char *arg = kstrbuf + kstroff_arr[i];
                int slen = strlen(arg) + 1;
                if (out + slen > (int)sizeof(current_proc->cmdline))
                    break;
                memcpy(current_proc->cmdline + out, arg, slen);
                out += slen;
            }
        }
        if (out == 0) {
            int nl = strlen(current_proc->name);
            if (nl > (int)sizeof(current_proc->cmdline) - 1)
                nl = (int)sizeof(current_proc->cmdline) - 1;
            memcpy(current_proc->cmdline, current_proc->name, nl);
            current_proc->cmdline[nl] = '\0';
            out = nl + 1;
        }
        current_proc->cmdline_len = (uint16_t)out;
    }

    /* POSIX: close file descriptors marked with FD_CLOEXEC.
     * ENH: Check per-fd fd_flags (bit 0 = FD_CLOEXEC) in addition to the
     * legacy O_CLOEXEC check on the open file flags, so that both old and
     * new paths are covered. */
    {
        #define KERN_O_CLOEXEC 02000000 /* matches libc O_CLOEXEC */
        struct open_file **oft = proc_ofile(current_proc);
        for (int fd = 0; fd < NOFILE; fd++) {
            struct open_file *f = oft[fd];
            if (f && !vfs_file_valid(f)) {
                oft[fd] = NULL;
                current_proc->fd_flags[fd] = 0;
                continue;
            }
            if (f && ((f->flags & KERN_O_CLOEXEC) ||
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
        for (int i = 1; i < 65; i++) {
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
        int execfn_len   = (int)strlen(resolved) + 1;
        int random_len   = 16;
        uint64_t auxv[EXECVE_AUXV_PAIRS * 2];
        int auxc = 0;
        int header       = 8;
        int auxv_sz;
        int total;

        /* BF: External musl binaries scan a Linux-style auxv immediately
         * after envp[].  Without a real AT_NULL-terminated vector, they
         * parse argv string bytes as aux entries and misinitialize startup
         * state such as page size and canary pointers. */
        auxv[auxc * 2] = AT_PAGESZ;
        auxv[auxc * 2 + 1] = PAGE_SIZE;
        auxc++;
        auxv[auxc * 2] = AT_ENTRY;
        auxv[auxc * 2 + 1] = newtf->epc;
        auxc++;
        auxv[auxc * 2] = AT_UID;
        auxv[auxc * 2 + 1] = (uint64_t)current_proc->uid;
        auxc++;
        auxv[auxc * 2] = AT_EUID;
        auxv[auxc * 2 + 1] = (uint64_t)current_proc->uid;
        auxc++;
        auxv[auxc * 2] = AT_GID;
        auxv[auxc * 2 + 1] = (uint64_t)current_proc->gid;
        auxc++;
        auxv[auxc * 2] = AT_EGID;
        auxv[auxc * 2 + 1] = (uint64_t)current_proc->gid;
        auxc++;
        auxv[auxc * 2] = AT_SECURE;
        auxv[auxc * 2 + 1] = 0;
        auxc++;
        auxv[auxc * 2] = AT_RANDOM;
        auxv[auxc * 2 + 1] = 0; /* filled in after stack layout is fixed */
        auxc++;
        auxv[auxc * 2] = AT_EXECFN;
        auxv[auxc * 2 + 1] = 0; /* filled in after stack layout is fixed */
        auxc++;
        auxv[auxc * 2] = AT_NULL;
        auxv[auxc * 2 + 1] = 0;
        auxc++;

        auxv_sz = auxc * 2 * 8;
        total = header + argv_ptrs_sz + envp_ptrs_sz + auxv_sz +
                kstroff + kenvoff + execfn_len + random_len;
        total = (total + 15) & ~15;

        uint64_t sp            = USER_STACK_TOP - 32 - total;
        uint64_t argv_ptr_base = sp + header;
        uint64_t envp_ptr_base = sp + header + argv_ptrs_sz;
        uint64_t auxv_base     = sp + header + argv_ptrs_sz + envp_ptrs_sz;
        uint64_t argv_str_base = auxv_base + auxv_sz;
        uint64_t envp_str_base = argv_str_base + kstroff;
        uint64_t execfn_base   = envp_str_base + kenvoff;
        uint64_t random_base   = execfn_base + execfn_len;
        uint64_t random_words[2];

        random_words[0] = r_time() ^ ((uint64_t)current_proc->pid << 32);
        random_words[1] = r_time() ^ newtf->epc ^ execfn_base;
        auxv[7 * 2 + 1] = random_base;
        auxv[8 * 2 + 1] = execfn_base;

        /* BF: Use multi-chunk copyout to avoid kbuf overflow.
         * The old code copied header + pointer arrays + all strings into a
         * single 4096-byte kalloc page, but total could exceed that when
         * argv and envp strings fill their respective pages.  Now we copy
         * only the header + pointer arrays + auxv into kbuf (still < 1KB),
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
            memcpy(kbuf + off, auxv, auxv_sz);
            off += auxv_sz;

            /* Multi-chunk copyout: header+ptrs+auxv first, then string data. */
            copyout(current_proc->pagetable, sp, kbuf, off);
            if (kstroff > 0)
                copyout(current_proc->pagetable, argv_str_base, kstrbuf, kstroff);
            if (kenvoff > 0)
                copyout(current_proc->pagetable, envp_str_base, kenvbuf, kenvoff);
            copyout(current_proc->pagetable, execfn_base, resolved, execfn_len);
            copyout(current_proc->pagetable, random_base, random_words, random_len);
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
    /* BF: Release a blocked vfork parent only after execve has finished
     * rebuilding argv/envp/auxv so the child is ready to enter user mode. */
    release_vfork_parent(current_proc);
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
        return (long)-EFAULT; /* EFAULT */
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
    return (long)sys_wait4(wpid, tf->a1, options);
}

long sys_waitid_handler(struct trapframe *tf) {
    int idtype = (int)tf->a0;
    pid_t id = (pid_t)(int)tf->a1;
    uint64_t info_uaddr = tf->a2;
    int options = (int)tf->a3;
    int32_t zero = 0;

    if (idtype < 0 || idtype > 2)
        return (long)-22; /* -EINVAL */

    if ((options & (4 | 16)) == 0)
        options |= 4; /* WEXITED by default for existing tests/libc callers */

    for (;;) {
        int found_child = 0;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            uint8_t kinfo[K_SIGINFO_SIZE];
            int report_exit = 0;
            int report_stop = 0;

            if (p->state == UNUSED || p->parent != current_proc)
                continue;

            if (idtype == 1 && p->pid != id)
                continue;
            if (idtype == 2 && p->pgid != id)
                continue;

            found_child = 1;
            report_exit = (options & 4) && p->state == ZOMBIE;
            report_stop = (options & (2 | 16)) && p->stopped_signal;
            if (!report_exit && !report_stop)
                continue;

            memset(kinfo, 0, sizeof(kinfo));
            *(int32_t *)(kinfo + 0) = SIGCHLD;
            if (report_exit) {
                *(int32_t *)(kinfo + 8) = p->killed_by_signal ? K_CLD_KILLED
                                                              : K_CLD_EXITED;
                *(int32_t *)(kinfo + 12) = (int32_t)p->pid;
                *(int32_t *)(kinfo + 20) = p->killed_by_signal
                                               ? (p->killed_by_signal & 0x7f)
                                               : (p->exit_status & 0xff);
            } else {
                *(int32_t *)(kinfo + 8) = K_CLD_STOPPED;
                *(int32_t *)(kinfo + 12) = (int32_t)p->pid;
                *(int32_t *)(kinfo + 20) = p->stopped_signal;
            }

            if (info_uaddr &&
                copyout(current_proc->pagetable, info_uaddr, kinfo, sizeof(kinfo)) < 0)
                return (long)-EFAULT;

            if (report_exit && !(options & 0x01000000)) { /* WNOWAIT */
                reap_zombie(p);
            } else if (report_stop) {
                p->stopped_signal = 0;
            }
            return 0;
        }

        if (!found_child)
            return (long)-10; /* -ECHILD */

        if (options & 1) { /* WNOHANG */
            if (info_uaddr &&
                copyout(current_proc->pagetable, info_uaddr, &zero, sizeof(zero)) < 0)
                return (long)-EFAULT;
            return 0;
        }

        if (current_proc) {
            current_proc->waiting_for_uart = 0;
            current_proc->waiting_for_child = 1;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
            current_proc->waiting_for_child = 0;
            {
                uint64_t deliverable = signal_deliverable_mask(current_proc);
                struct kern_sigaction *ksa = proc_sigactions(current_proc);
                for (int s = 1; s < 65 && deliverable; s++) {
                    if (!(deliverable & signal_bit(s)))
                        continue;
                    if (ksa[s].handler > 1) {
                        if (s == SIGCHLD)
                            continue;
                        return -4; /* -EINTR */
                    }
                    if (ksa[s].handler == 0) {
                        if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                            continue;
                        if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                            continue;
                        if (s == SIGCONT)
                            continue;
                        return -4; /* -EINTR */
                    }
                }
            }
        }
    }
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
    proc_exit_current((int)tf->a0, 0);
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
                return (long)-EFAULT;
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
