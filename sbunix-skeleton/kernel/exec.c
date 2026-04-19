#include <exec.h>
#include <printk.h>
#include <string.h>
#include <tar.h>
#include <elf.h>
#include <kalloc.h>
#include <kmalloc.h>
#include <riscv.h>
#include <vm.h>
#include <vfs.h>
#include <proc.h>
#include <syscall.h>

extern char _tarfs_start[];
extern char _tarfs_end[];
extern char _kernel_end[];

/* ENH: SIGTRAMP_VA is defined in riscv.h — used here by map_sigtrampoline */

/* ENH: Allocate and map the signal trampoline page into a user page table.
 * This is shared by exec() and exec_vfs() to avoid duplicating the same
 * allocation + mapping + error-handling logic. */
static int map_sigtrampoline(pagetable_t pt) {
  extern char sigtrampoline[], sigtrampoline_end[];
  if ((uint64_t)(sigtrampoline_end - sigtrampoline) > PAGE_SIZE)
    return -1;
  char *tramp_page = kalloc();
  if (!tramp_page) return -1;
  memset(tramp_page, 0, PAGE_SIZE);
  memcpy(tramp_page, sigtrampoline, sigtrampoline_end - sigtrampoline);
  if (mappages(pt, SIGTRAMP_VA, PAGE_SIZE, (uint64_t)tramp_page,
               PTE_R | PTE_X | PTE_U) < 0) {
    kfree(tramp_page);
    return -1;
  }
  return 0;
}

static void trapframe_prepare_user_return(struct trapframe *tf) {
  uint64_t sstatus = r_sstatus();
  sstatus &= ~SSTATUS_SPP;
  sstatus |= SSTATUS_SPIE;
  tf->saved_sstatus = sstatus;
  tf->return_to_user = 1;
}

static uint64_t exec_mmap_base(uint64_t sz) {
    uint64_t base = PGROUNDUP(sz) + PAGE_SIZE;
    if (base < USER_MMAP_FLOOR)
        base = USER_MMAP_FLOOR;
    return base;
}

/* BF: Clean up staged exec state on failure — unmap pages from
 * staged pagetable and free the VMA chain built during loading.
 * This allows exec to roll back without corrupting the original
 * process state. */
static void staged_exec_cleanup(pagetable_t pt, struct vm_area *staged_vmas) {
  struct vm_area *v = staged_vmas;
  while (v) {
    uvmunmap(pt, v->start, v->end - v->start);
    v = v->next;
  }
  proc_freepagetable(pt);
  vma_free_chain(staged_vmas);
}

int exec(char *path) {
    struct posix_header *elf_tar_header;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    char *elf_data;
    unsigned int i;
    pagetable_t pagetable;
    uint64_t sz = 0;
    struct trapframe *tf = current_proc->trapframe;

    // 1. Find ELF
    elf_tar_header = tar_find_file(_tarfs_start, _tarfs_end, path);
    if (!elf_tar_header) return -1;

    elf_data = (char *)elf_tar_header + sizeof(struct posix_header);
    ehdr = (Elf64_Ehdr *)elf_data;

    // 2. Validate ELF magic — fail early before any state changes
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) return -1;

    /* BF: Create new pagetable BEFORE destroying old state — this is the
     * core of the failure-atomic exec design.  If the new image fails to
     * load, we can discard the new pagetable and keep the old process. */
    pagetable = uvmcreate();
    if (pagetable == 0) return -1;

    /* BF: Save old state so we can restore on failure.  Temporarily swap
     * vma_list to an empty staging list.  Disable SIE while vma_list
     * points to staging, so timer interrupts cannot see inconsistent state. */
    pagetable_t old_pagetable = current_proc->pagetable;
    struct vm_area *old_vma_list = current_proc->vma_list;
    uint64_t old_sz = current_proc->sz;

    unsigned long old_sstatus = r_sstatus();
    w_sstatus(old_sstatus & ~SSTATUS_SIE);
    current_proc->vma_list = NULL;  /* staging starts empty */

    // 3. Load PT_LOAD segments into new pagetable
    for (i = 0; i < ehdr->e_phnum; i++) {
        phdr = (Elf64_Phdr *)(elf_data + ehdr->e_phoff + i*sizeof(Elf64_Phdr));
        if (phdr->p_type == PT_LOAD) {
            /* Compute PTE flags from ELF p_flags; keep PTE_W during load for copyout */
            int pte_flags = 0;
            if (phdr->p_flags & 0x4) pte_flags |= PTE_R;  /* PF_R */
            if (phdr->p_flags & 0x2) pte_flags |= PTE_W;  /* PF_W */
            if (phdr->p_flags & 0x1) pte_flags |= PTE_X;  /* PF_X */
            if(uvmalloc(pagetable, phdr->p_vaddr, phdr->p_vaddr + phdr->p_memsz, pte_flags | PTE_W) == 0) goto bad;
            if(copyout(pagetable, phdr->p_vaddr, elf_data + phdr->p_offset, phdr->p_filesz) < 0) goto bad;

            /* Remove PTE_W from read-only segments (e.g., text) after loading.
             * This prevents text pages from being COW-marked during fork,
             * which would allow COW resolution to make text writable and
             * potentially corrupt code. */
            if (!(phdr->p_flags & 0x2)) { /* PF_W not set -- read-only segment */
                uint64_t seg_start = PGROUNDDOWN(phdr->p_vaddr);
                uint64_t seg_end_pg = PGROUNDUP(phdr->p_vaddr + phdr->p_memsz);
                for (uint64_t a = seg_start; a < seg_end_pg; a += PAGE_SIZE) {
                    pte_t *ppte = walk(pagetable, a, 0);
                    if (ppte && (*ppte & PTE_V))
                        pte_update(pagetable, a, ppte, (*ppte & ~PTE_W),
                                   0, PTE_UPDATE_EXEC_PERM,
                                   PTE_SFENCE_NONE);
                }
            }

            uint64_t seg_end = phdr->p_vaddr + phdr->p_memsz;
            if (seg_end > sz) sz = seg_end;

            /* Compute VMA protection bits from ELF phdr->p_flags */
            int vprot = 0;
            if (phdr->p_flags & 0x4) vprot |= PROT_READ;   /* PF_R */
            if (phdr->p_flags & 0x2) vprot |= PROT_WRITE;  /* PF_W */
            if (phdr->p_flags & 0x1) vprot |= PROT_EXEC;   /* PF_X */
            uint64_t vma_start = PGROUNDDOWN(phdr->p_vaddr);
            uint64_t vma_end   = PGROUNDUP(phdr->p_vaddr + phdr->p_memsz);
            /* BF: vma_add adds to current_proc->vma_list which is now
             * the staging list (NULL at start).  On failure, we restore
             * old_vma_list and free these staged VMAs. */
            vma_add(current_proc, vma_start, vma_end, vprot, MAP_PRIVATE);
        }
    }

    // 4. User Stack -- allocate 8 pages (32 KB) below the trapframe/trampoline
    #define USER_STACK_PAGES 8
    uint64_t stack_va = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
    char *top_page_mem = NULL;
    for (int pg = 0; pg < USER_STACK_PAGES; pg++) {
        char *sp_mem = kalloc();
        if (!sp_mem) goto bad;
        memset(sp_mem, 0, PAGE_SIZE);
        uint64_t vaddr = stack_va + pg * PAGE_SIZE;
        if (uvmmap(pagetable, vaddr, (uint64_t)sp_mem, PAGE_SIZE, PTE_R | PTE_W | PTE_U) < 0) {
            kfree(sp_mem); /* BF: Free page not yet attached to pagetable */
            goto bad;
        }
        if (pg == USER_STACK_PAGES - 1)
            top_page_mem = sp_mem;  /* remember physical addr of topmost page */
    }

    /* Add a VMA for the user stack region (up to USER_STACK_TOP, not MAXVA) */
    vma_add(current_proc, stack_va, USER_STACK_TOP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON);

    // 5. Map trampoline + trapframe pages (NO kernel identity mapping needed --
    //    the trampoline switches satp to kernel_pagetable on trap entry)
    if(uvmmap_kernel(pagetable) < 0) goto bad;

    /* ENH: Map signal trampoline page via extracted helper (replaces inline block) */
    if (map_sigtrampoline(pagetable) < 0) goto bad;
    /* VMA for sigtrampoline — enables proper cleanup on exit/exec */
    vma_add(current_proc, SIGTRAMP_VA, SIGTRAMP_VA + PAGE_SIZE,
            PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON);

    /* === POINT OF NO RETURN ===
     * Everything above succeeded.  Now commit the new state and tear down old. */

    /* BF: Determine shared_pt status before detaching, so we know whether
     * to free the old pagetable later. */
    int old_pt_still_shared = 0;

    /* Detach from shared pagetable if this is a CLONE_VM thread */
    if (current_proc->shared_pt) {
        current_proc->shared_pt->refcount--;
        if (current_proc->shared_pt->refcount == 0) {
            /* Last user of this shared pagetable wrapper. */
            kmfree(current_proc->shared_pt, sizeof(struct shared_pagetable));
        } else {
            old_pt_still_shared = 1;
        }
        current_proc->shared_pt = NULL;
        current_proc->clone_flags &= ~CLONE_VM;
    }

    /* Detach from shared signal handlers (exec resets all to SIG_DFL) */
    if (current_proc->shared_sa) {
        current_proc->shared_sa->refcount--;
        if (current_proc->shared_sa->refcount == 0)
            kmfree(current_proc->shared_sa, sizeof(struct shared_sigactions));
        current_proc->shared_sa = NULL;
        current_proc->clone_flags &= ~CLONE_SIGHAND;
    }
    /* BF: POSIX requires SIG_IGN to be preserved across exec.
     * Reset user-installed handlers (handler >= 2) to SIG_DFL.
     * Clear flags/mask/restorer for all signals -- stale flags
     * like SA_NOCLDWAIT must not persist across exec.
     * This MUST happen after successful load, not before. */
    for (int si = 0; si < 32; si++) {
        if (current_proc->sigactions[si].handler >= 2) {
            current_proc->sigactions[si].handler = 0;  /* SIG_DFL */
        }
        current_proc->sigactions[si].mask = 0;
        current_proc->sigactions[si].flags = 0;
        current_proc->sigactions[si].restorer = 0;
    }

    /* BF: Free old VMAs now that we're past the point of no return.
     * The staging list (current_proc->vma_list) becomes the new live list. */
    vma_free_chain(old_vma_list);

    // 6. Update process state
    current_proc->pagetable = pagetable;
    current_proc->sz = sz;

    // Initialize stack bounds for auto-grow
    current_proc->stack_top = USER_STACK_TOP;
    current_proc->stack_bottom = stack_va;
    current_proc->mmap_base = exec_mmap_base(sz);

    /* Update process name from the last component of the path */
    {
        const char *base = path;
        const char *p = path;
        while (*p) {
            if (*p == '/')
                base = p + 1;
            p++;
        }
        int ni = 0;
        while (base[ni] && ni < (int)sizeof(current_proc->name) - 1) {
            current_proc->name[ni] = base[ni];
            ni++;
        }
        current_proc->name[ni] = '\0';
    }

    /* Record executable path for /proc/<pid>/exe */
    {
        int ei = 0;
        while (path[ei] && ei < (int)sizeof(current_proc->exe_path) - 1) {
            current_proc->exe_path[ei] = path[ei];
            ei++;
        }
        current_proc->exe_path[ei] = '\0';
    }

    /* Switch to new page table before freeing old one */
    w_satp(MAKE_SATP(pagetable));
    sfence_vma();
    asm volatile("fence.i");

    /* Re-enable SIE now that all state is consistent */
    w_sstatus(old_sstatus);

    // Reclaim old user pages + page table pages.
    // Kernel stays on kernel_pagetable (user PT has no kernel mappings).
    if (old_pagetable != kernel_pagetable && !old_pt_still_shared)
        proc_freepagetable(old_pagetable);

    // 7. Initialize trapframe
    memset(tf, 0, sizeof(*tf));
    tf->epc = ehdr->e_entry;
    trapframe_prepare_user_return(tf);

    // Set up initial stack: crt.S expects [argc, argv..., NULL, envp..., NULL]
    // For no args/env: [0, NULL, NULL] = 24 bytes, padded to 32 for alignment
    uint64_t *stack_top_mem = (uint64_t *)(top_page_mem + PAGE_SIZE);
    stack_top_mem[-1] = 0;  // padding
    stack_top_mem[-2] = 0;  // envp[0] = NULL
    stack_top_mem[-3] = 0;  // argv[0] = NULL
    stack_top_mem[-4] = 0;  // argc = 0
    tf->sp = USER_STACK_TOP - 32;  // stack top is just below user stack top

    return 0;

bad:
    /* BF: Restore original process state on failure -- this is the
     * rollback path.  Discard the staged VMAs and new pagetable,
     * put back the old vma_list, and re-enable SIE. */
    {
        struct vm_area *staged = current_proc->vma_list;
        current_proc->vma_list = old_vma_list;
        current_proc->sz = old_sz;
        w_sstatus(old_sstatus);
        staged_exec_cleanup(pagetable, staged);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* exec_vfs: load an ELF binary from any VFS-mounted filesystem       */
/* ------------------------------------------------------------------ */
int exec_vfs(const char *resolved_path) {
    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved_path, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops || !ops->open || !ops->read)
        return -1;

    void *priv = NULL;
    if (ops->open(mnt->fs_data, relpath, 0, &priv) < 0)
        return -1;

    /* 1. Read ELF header -- fail early before any state changes */
    Elf64_Ehdr ehdr;
    int64_t off = 0;
    int64_t nr = ops->read(mnt->fs_data, priv, (char *)&ehdr, sizeof(ehdr), &off);
    if (nr < (int64_t)sizeof(ehdr))
        goto fail_close;

    /* 2. Validate ELF -- fail early before any state changes */
    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3)
        goto fail_close;

    /* 3. Read program headers */
    uint64_t phdr_total = (uint64_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
    if (phdr_total > PAGE_SIZE)
        goto fail_close;

    char *phdr_buf = kalloc();
    if (!phdr_buf)
        goto fail_close;

    off = (int64_t)ehdr.e_phoff;
    nr = ops->read(mnt->fs_data, priv, phdr_buf, phdr_total, &off);
    if (nr < (int64_t)phdr_total) {
        kfree(phdr_buf);
        goto fail_close;
    }

    /* BF: Create new pagetable BEFORE destroying old state. */
    pagetable_t pagetable = uvmcreate();
    if (pagetable == 0) {
        kfree(phdr_buf);
        goto fail_close;
    }

    /* BF: Save old state for rollback.  Swap vma_list to empty staging
     * list with SIE disabled so timer interrupts see consistent state. */
    pagetable_t old_pagetable = current_proc->pagetable;
    struct vm_area *old_vma_list = current_proc->vma_list;
    uint64_t old_sz = current_proc->sz;

    unsigned long old_sstatus = r_sstatus();
    w_sstatus(old_sstatus & ~SSTATUS_SIE);
    current_proc->vma_list = NULL;  /* staging starts empty */

    /* 4. Load PT_LOAD segments */
    uint64_t sz = 0;
    struct trapframe *tf = current_proc->trapframe;
    char *tmp_page = kalloc();
    if (!tmp_page) {
        kfree(phdr_buf);
        goto bad_vfs;
    }

    for (unsigned int i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(phdr_buf + i * sizeof(Elf64_Phdr));
        if (ph->p_type != PT_LOAD)
            continue;

        int pte_flags = 0;
        if (ph->p_flags & PF_R) pte_flags |= PTE_R;
        if (ph->p_flags & PF_W) pte_flags |= PTE_W;
        if (ph->p_flags & PF_X) pte_flags |= PTE_X;

        if (uvmalloc(pagetable, ph->p_vaddr,
                     ph->p_vaddr + ph->p_memsz,
                     pte_flags | PTE_W) == 0) {
            kfree(tmp_page);
            kfree(phdr_buf);
            goto bad_vfs;
        }

        /* Read segment data page-by-page from VFS */
        uint64_t remain = ph->p_filesz;
        uint64_t seg_va = ph->p_vaddr;
        int64_t file_off = (int64_t)ph->p_offset;

        while (remain > 0) {
            uint64_t page_off = seg_va & (PAGE_SIZE - 1);
            uint64_t chunk = PAGE_SIZE - page_off;
            if (chunk > remain) chunk = remain;

            nr = ops->read(mnt->fs_data, priv, tmp_page, chunk, &file_off);
            if (nr < (int64_t)chunk) {
                kfree(tmp_page);
                kfree(phdr_buf);
                goto bad_vfs;
            }
            if (copyout(pagetable, seg_va, tmp_page, chunk) < 0) {
                kfree(tmp_page);
                kfree(phdr_buf);
                goto bad_vfs;
            }
            seg_va += chunk;
            remain -= chunk;
        }

        /* Strip PTE_W from read-only segments after loading */
        if (!(ph->p_flags & PF_W)) {
            uint64_t a;
            for (a = PGROUNDDOWN(ph->p_vaddr);
                 a < PGROUNDUP(ph->p_vaddr + ph->p_memsz);
                 a += PAGE_SIZE) {
                pte_t *ppte = walk(pagetable, a, 0);
                if (ppte && (*ppte & PTE_V))
                    pte_update(pagetable, a, ppte, (*ppte & ~PTE_W),
                               0, PTE_UPDATE_EXEC_PERM,
                               PTE_SFENCE_NONE);
            }
        }

        uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
        if (seg_end > sz) sz = seg_end;

        /* RF: Use symbolic PROT_* / MAP_* constants instead of raw integers */
        int vprot = 0;
        /* ENH: Use symbolic PROT_* constants instead of raw hex flags */
        if (ph->p_flags & PF_R) vprot |= PROT_READ;
        if (ph->p_flags & PF_W) vprot |= PROT_WRITE;
        if (ph->p_flags & PF_X) vprot |= PROT_EXEC;
        /* BF: vma_add adds to staging vma_list (currently NULL -> growing).
         * On failure we restore old_vma_list and discard staged VMAs. */
        vma_add(current_proc, PGROUNDDOWN(ph->p_vaddr),
                PGROUNDUP(ph->p_vaddr + ph->p_memsz),
                vprot, MAP_PRIVATE);
    }
    kfree(tmp_page);
    kfree(phdr_buf);
    if (ops->close) ops->close(mnt->fs_data, priv);
    priv = NULL;

    /* 5. User stack (8 pages) */
    #define USER_STACK_PAGES_VFS 8
    uint64_t stack_va = USER_STACK_TOP - (USER_STACK_PAGES_VFS * PAGE_SIZE);
    char *top_page_mem_vfs = NULL;
    for (int pg = 0; pg < USER_STACK_PAGES_VFS; pg++) {
        char *sp_mem = kalloc();
        if (!sp_mem) goto bad_vfs;
        memset(sp_mem, 0, PAGE_SIZE);
        if (mappages(pagetable, stack_va + pg * PAGE_SIZE, PAGE_SIZE,
                     (uint64_t)sp_mem, PTE_R | PTE_W | PTE_U) < 0) {
            kfree(sp_mem); /* BF: Free page not yet attached to pagetable */
            goto bad_vfs;
        }
        if (pg == USER_STACK_PAGES_VFS - 1)
            top_page_mem_vfs = sp_mem;
    }
    /* RF: Use symbolic constants instead of raw hex flags for readability.
     * Values verified against libc/include/sys/mman.h. */
    vma_add(current_proc, stack_va, USER_STACK_TOP,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON);

    /* 6. Map kernel entries */
    if (uvmmap_kernel(pagetable) < 0) goto bad_vfs;

    /* ENH: Map signal trampoline page via extracted helper (replaces inline block) */
    if (map_sigtrampoline(pagetable) < 0) goto bad_vfs;
    /* VMA for sigtrampoline — enables proper cleanup on exit/exec */
    vma_add(current_proc, SIGTRAMP_VA, SIGTRAMP_VA + PAGE_SIZE,
            PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON);

    /* === POINT OF NO RETURN ===
     * Everything above succeeded.  Now commit new state and tear down old. */

    /* BF: Determine shared_pt status before detaching. */
    int old_pt_still_shared = 0;

    /* Detach from shared state -- AFTER successful load */
    if (current_proc->shared_pt) {
        current_proc->shared_pt->refcount--;
        if (current_proc->shared_pt->refcount == 0)
            kmfree(current_proc->shared_pt, sizeof(struct shared_pagetable));
        else
            old_pt_still_shared = 1;
        current_proc->shared_pt = NULL;
        current_proc->clone_flags &= ~0x00000100; /* CLONE_VM */
    }
    if (current_proc->shared_sa) {
        current_proc->shared_sa->refcount--;
        if (current_proc->shared_sa->refcount == 0)
            kmfree(current_proc->shared_sa, sizeof(struct shared_sigactions));
        current_proc->shared_sa = NULL;
        current_proc->clone_flags &= ~0x00000800; /* CLONE_SIGHAND */
    }
    /* BF: Signal handler reset MUST happen after successful load.
     * POSIX: SIG_IGN preserved, user handlers reset to SIG_DFL. */
    for (int si = 0; si < 32; si++) {
        if (current_proc->sigactions[si].handler >= 2) {
            current_proc->sigactions[si].handler = 0;  /* SIG_DFL */
        }
        current_proc->sigactions[si].mask = 0;
        current_proc->sigactions[si].flags = 0;
        current_proc->sigactions[si].restorer = 0;
    }

    /* BF: Free old VMAs now that load succeeded. */
    vma_free_chain(old_vma_list);

    /* 7. Update process */
    current_proc->pagetable = pagetable;
    current_proc->sz = sz;
    current_proc->stack_top = USER_STACK_TOP;
    current_proc->stack_bottom = stack_va;
    current_proc->mmap_base = exec_mmap_base(sz);

    /* Update process name and exe_path */
    {
        const char *base = resolved_path;
        for (const char *p = resolved_path; *p; p++)
            if (*p == '/') base = p + 1;
        int ni = 0;
        while (base[ni] && ni < (int)sizeof(current_proc->name) - 1) {
            current_proc->name[ni] = base[ni]; ni++;
        }
        current_proc->name[ni] = '\0';
    }
    {
        int ei = 0;
        while (resolved_path[ei] && ei < (int)sizeof(current_proc->exe_path) - 1) {
            current_proc->exe_path[ei] = resolved_path[ei]; ei++;
        }
        current_proc->exe_path[ei] = '\0';
    }

    w_satp(MAKE_SATP(pagetable));
    sfence_vma();
    asm volatile("fence.i");

    /* Re-enable SIE now that all state is consistent */
    w_sstatus(old_sstatus);

    if (old_pagetable != kernel_pagetable && !old_pt_still_shared)
        proc_freepagetable(old_pagetable);

    /* 8. Initialize trapframe */
    memset(tf, 0, sizeof(*tf));
    tf->epc = ehdr.e_entry;
    trapframe_prepare_user_return(tf);

    uint64_t *stp = (uint64_t *)(top_page_mem_vfs + PAGE_SIZE);
    stp[-1] = 0; stp[-2] = 0; stp[-3] = 0; stp[-4] = 0;
    tf->sp = USER_STACK_TOP - 32;

    return 0;

bad_vfs:
    /* BF: Restore original process state on failure -- rollback path.
     * Discard staged VMAs and new pagetable, restore old vma_list. */
    {
        struct vm_area *staged = current_proc->vma_list;
        current_proc->vma_list = old_vma_list;
        current_proc->sz = old_sz;
        w_sstatus(old_sstatus);
        staged_exec_cleanup(pagetable, staged);
    }
    if (priv && ops->close) ops->close(mnt->fs_data, priv);
    return -1;

fail_close:
    if (ops->close) ops->close(mnt->fs_data, priv);
    return -1;
}
