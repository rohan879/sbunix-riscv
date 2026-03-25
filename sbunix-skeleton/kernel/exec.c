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

    // 2. Validate
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) return -1;

    /* Detach from shared pagetable if this is a CLONE_VM thread */
    if (current_proc->shared_pt) {
        current_proc->shared_pt->refcount--;
        if (current_proc->shared_pt->refcount == 0) {
            /* We were the last user -- free old shared pagetable wrapper.
             * The actual pagetable pages leak (pre-existing issue). */
            kmfree(current_proc->shared_pt, sizeof(struct shared_pagetable));
        }
        current_proc->shared_pt = NULL;
        current_proc->clone_flags &= ~CLONE_VM;
        /* exec will create a brand new pagetable below */
    }

    /* Detach from shared signal handlers (exec resets all to SIG_DFL) */
    if (current_proc->shared_sa) {
        current_proc->shared_sa->refcount--;
        if (current_proc->shared_sa->refcount == 0)
            kmfree(current_proc->shared_sa, sizeof(struct shared_sigactions));
        current_proc->shared_sa = NULL;
        current_proc->clone_flags &= ~CLONE_SIGHAND;
    }
    /* Reset all sigactions to default */
    memset(current_proc->sigactions, 0, sizeof(current_proc->sigactions));

    /* Clear old VMAs before loading new binary */
    vma_free_all(current_proc);

    pagetable = uvmcreate();
    if(pagetable == 0) return -1;

    // 3. Load PT_LOAD segments
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
            if (!(phdr->p_flags & 0x2)) { /* PF_W not set — read-only segment */
                uint64_t seg_start = PGROUNDDOWN(phdr->p_vaddr);
                uint64_t seg_end_pg = PGROUNDUP(phdr->p_vaddr + phdr->p_memsz);
                for (uint64_t a = seg_start; a < seg_end_pg; a += PAGE_SIZE) {
                    pte_t *ppte = walk(pagetable, a, 0);
                    if (ppte && (*ppte & PTE_V))
                        *ppte = (*ppte & ~PTE_W);
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
            vma_add(current_proc, vma_start, vma_end, vprot, MAP_PRIVATE);
        }
    }

    // 4. User Stack — allocate 8 pages (32 KB) below the trapframe/trampoline
    #define USER_STACK_PAGES 8
    uint64_t stack_va = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
    char *top_page_mem = NULL;
    for (int pg = 0; pg < USER_STACK_PAGES; pg++) {
        char *sp_mem = kalloc();
        if (!sp_mem) goto bad;
        uint64_t vaddr = stack_va + pg * PAGE_SIZE;
        if (uvmmap(pagetable, vaddr, (uint64_t)sp_mem, PAGE_SIZE, PTE_R | PTE_W | PTE_U) < 0) goto bad;
        if (pg == USER_STACK_PAGES - 1)
            top_page_mem = sp_mem;  /* remember physical addr of topmost page */
    }
    
    /* Add a VMA for the user stack region (up to USER_STACK_TOP, not MAXVA) */
    vma_add(current_proc, stack_va, USER_STACK_TOP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON);

    // 5. Map trampoline + trapframe pages (NO kernel identity mapping needed —
    //    the trampoline switches satp to kernel_pagetable on trap entry)
    if(uvmmap_kernel(pagetable) < 0) goto bad;

    // 6. Update process state
    pagetable_t old_pt = current_proc->pagetable;
    current_proc->pagetable = pagetable;
    current_proc->sz = sz;

    // Initialize stack bounds for auto-grow
    current_proc->stack_top = USER_STACK_TOP;
    current_proc->stack_bottom = stack_va;

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

    // Reclaim old user pages + page table pages.
    // Kernel stays on kernel_pagetable (user PT has no kernel mappings).
    if (old_pt != kernel_pagetable)
        proc_freepagetable(old_pt);

    // 7. Initialize trapframe
    memset(tf, 0, sizeof(*tf));
    tf->epc = ehdr->e_entry;

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
    /* The new pagetable may contain kernel identity maps — we cannot
     * call uvmunmap on the whole range without corrupting kernel memory.
     * Accept the leak on the (rare) exec failure path. */
    return -1;
}

/* ------------------------------------------------------------------ */
/* exec_vfs: load an ELF binary from any VFS-mounted filesystem       */
/* ------------------------------------------------------------------ */
int exec_vfs(const char *resolved_path) {
    const char *relpath;
    struct vfs_ops *ops = vfs_lookup_mount(resolved_path, &relpath);
    if (!ops || !ops->open || !ops->read)
        return -1;

    void *priv = NULL;
    if (ops->open(relpath, 0, &priv) < 0)
        return -1;

    /* 1. Read ELF header */
    Elf64_Ehdr ehdr;
    int64_t off = 0;
    int64_t nr = ops->read(priv, (char *)&ehdr, sizeof(ehdr), &off);
    if (nr < (int64_t)sizeof(ehdr))
        goto fail_close;

    /* 2. Validate ELF */
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
    nr = ops->read(priv, phdr_buf, phdr_total, &off);
    if (nr < (int64_t)phdr_total) {
        kfree(phdr_buf);
        goto fail_close;
    }

    /* Detach from shared state (same as exec()) */
    if (current_proc->shared_pt) {
        current_proc->shared_pt->refcount--;
        if (current_proc->shared_pt->refcount == 0)
            kmfree(current_proc->shared_pt, sizeof(struct shared_pagetable));
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
    memset(current_proc->sigactions, 0, sizeof(current_proc->sigactions));
    vma_free_all(current_proc);

    pagetable_t pagetable = uvmcreate();
    if (pagetable == 0) {
        kfree(phdr_buf);
        goto fail_close;
    }

    /* 4. Load PT_LOAD segments */
    uint64_t sz = 0;
    struct trapframe *tf = current_proc->trapframe;
    char *tmp_page = kalloc();
    if (!tmp_page) {
        kfree(phdr_buf);
        goto fail_close;
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

            nr = ops->read(priv, tmp_page, chunk, &file_off);
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
                    *ppte = (*ppte & ~PTE_W);
            }
        }

        uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
        if (seg_end > sz) sz = seg_end;

        int vprot = 0;
        if (ph->p_flags & PF_R) vprot |= 1; /* PROT_READ */
        if (ph->p_flags & PF_W) vprot |= 2; /* PROT_WRITE */
        if (ph->p_flags & PF_X) vprot |= 4; /* PROT_EXEC */
        vma_add(current_proc, PGROUNDDOWN(ph->p_vaddr),
                PGROUNDUP(ph->p_vaddr + ph->p_memsz),
                vprot, 0x02 /* MAP_PRIVATE */);
    }
    kfree(tmp_page);
    kfree(phdr_buf);
    if (ops->close) ops->close(priv);
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
                     (uint64_t)sp_mem, PTE_R | PTE_W | PTE_U) < 0)
            goto bad_vfs;
        if (pg == USER_STACK_PAGES_VFS - 1)
            top_page_mem_vfs = sp_mem;
    }
    vma_add(current_proc, stack_va, USER_STACK_TOP,
            1 | 2 /* PROT_READ | PROT_WRITE */, 0x02 | 0x20 /* MAP_PRIVATE | MAP_ANON */);

    /* 6. Map kernel entries */
    if (uvmmap_kernel(pagetable) < 0) goto bad_vfs;

    /* 7. Update process */
    pagetable_t old_pt = current_proc->pagetable;
    current_proc->pagetable = pagetable;
    current_proc->sz = sz;
    current_proc->stack_top = USER_STACK_TOP;
    current_proc->stack_bottom = stack_va;
    current_proc->mmap_base = PGROUNDUP(sz) + PAGE_SIZE;

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
    if (old_pt != kernel_pagetable)
        proc_freepagetable(old_pt);

    /* 8. Initialize trapframe */
    memset(tf, 0, sizeof(*tf));
    tf->epc = ehdr.e_entry;

    uint64_t *stp = (uint64_t *)(top_page_mem_vfs + PAGE_SIZE);
    stp[-1] = 0; stp[-2] = 0; stp[-3] = 0; stp[-4] = 0;
    tf->sp = USER_STACK_TOP - 32;

    return 0;

bad_vfs:
    if (priv && ops->close) ops->close(priv);
    return -1;

fail_close:
    if (ops->close) ops->close(priv);
    return -1;
}