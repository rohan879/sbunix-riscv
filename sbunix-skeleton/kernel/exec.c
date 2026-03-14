#include <exec.h>
#include <printk.h>
#include <string.h>
#include <tar.h>
#include <elf.h>
#include <kalloc.h>
#include <kmalloc.h>
#include <riscv.h>
#include <vm.h>
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
    pagetable_t pagetable = 0;
    pagetable_t old_pt = current_proc->pagetable;
    struct shared_pagetable *old_shared_pt = current_proc->shared_pt;
    struct shared_sigactions *old_shared_sa = current_proc->shared_sa;
    int old_clone_flags = current_proc->clone_flags;
    struct vm_area *old_vmas = current_proc->vma_list;
    struct vm_area *new_vmas = NULL;
    uint64_t sz = 0;
    unsigned long old_satp = 0;
    struct trapframe *tf = current_proc->trapframe;
    
    // 1. Find ELF
    elf_tar_header = tar_find_file(_tarfs_start, _tarfs_end, path);
    if (!elf_tar_header) return -1;

    elf_data = (char *)elf_tar_header + sizeof(struct posix_header);
    ehdr = (Elf64_Ehdr *)elf_data;

    // 2. Validate ELF magic
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) return -1;

    // 2b. Validate ELF header fields against tarfs entry bounds
    {
        size_t elf_size = tar_get_file_size(elf_tar_header);
        if (ehdr->e_phnum > 64) return -1; /* unreasonable phnum */
        uint64_t ph_end = (uint64_t)ehdr->e_phoff +
                          (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
        if (ph_end > elf_size) return -1; /* phdr table out of bounds */

        /* Pre-validate all PT_LOAD segments */
        for (unsigned int j = 0; j < ehdr->e_phnum; j++) {
            Elf64_Phdr *ph = (Elf64_Phdr *)(elf_data + ehdr->e_phoff +
                              j * sizeof(Elf64_Phdr));
            if (ph->p_type != PT_LOAD) continue;
            if (ph->p_offset + ph->p_filesz > elf_size) return -1;
            if (ph->p_filesz > ph->p_memsz) return -1;
            if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr) return -1; /* overflow */
            if (ph->p_vaddr + ph->p_memsz > MAXVA) return -1;
        }
    }

    pagetable = uvmcreate();
    if(pagetable == 0) return -1;
    old_satp = r_satp();
    w_satp(MAKE_SATP(kernel_pagetable));
    sfence_vma();

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
            uint64_t seg_end = phdr->p_vaddr + phdr->p_memsz;
            if (seg_end > sz) sz = seg_end;

            /* Compute VMA protection bits from ELF phdr->p_flags */
            int vprot = 0;
            if (phdr->p_flags & 0x4) vprot |= PROT_READ;   /* PF_R */
            if (phdr->p_flags & 0x2) vprot |= PROT_WRITE;  /* PF_W */
            if (phdr->p_flags & 0x1) vprot |= PROT_EXEC;   /* PF_X */
            uint64_t vma_start = PGROUNDDOWN(phdr->p_vaddr);
            uint64_t vma_end   = PGROUNDUP(phdr->p_vaddr + phdr->p_memsz);
            if (vma_add_list(&new_vmas, vma_start, vma_end, vprot, MAP_PRIVATE) < 0)
                goto bad;
        }
    }

    // 3b. Tighten PTE permissions — remove PTE_W added for copyout
    for (i = 0; i < ehdr->e_phnum; i++) {
        phdr = (Elf64_Phdr *)(elf_data + ehdr->e_phoff + i*sizeof(Elf64_Phdr));
        if (phdr->p_type == PT_LOAD) {
            int pte_flags = PTE_U;
            if (phdr->p_flags & 0x4) pte_flags |= PTE_R;
            if (phdr->p_flags & 0x2) pte_flags |= PTE_W;
            if (phdr->p_flags & 0x1) pte_flags |= PTE_X;
            uvmprotect(pagetable, phdr->p_vaddr, phdr->p_memsz, pte_flags);
        }
    }

    // 4. User Stack — allocate 16 pages (64 KB) to accommodate deep call stacks
    #define USER_STACK_PAGES 16
    uint64_t stack_va = MAXVA - (USER_STACK_PAGES * PAGE_SIZE);
    char *top_page_mem = NULL;
    for (int pg = 0; pg < USER_STACK_PAGES; pg++) {
        char *sp_mem = kalloc();
        if (!sp_mem) goto bad;
        uint64_t vaddr = stack_va + pg * PAGE_SIZE;
        if (uvmmap(pagetable, vaddr, (uint64_t)sp_mem, PAGE_SIZE, PTE_R | PTE_W | PTE_U) < 0) {
            kfree(sp_mem);
            goto bad;
        }
        if (pg == USER_STACK_PAGES - 1)
            top_page_mem = sp_mem;  /* remember physical addr of topmost page */
    }
    
    /* Add a VMA for the user stack region */
    if (vma_add_list(&new_vmas, stack_va, MAXVA, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON) < 0)
        goto bad;

    // 5. Identity map kernel and ALL RAM (No PTE_U)
    // This ensures kernel code, data, AND dynamically allocated pages (like kstack)
    // are accessible after switching satp.
    if(mappages_super(pagetable, 0x80000000, PHYSTOP - 0x80000000, 0x80000000, PTE_R | PTE_W | PTE_X) < 0) goto bad;
    // Map I/O registers needed for interrupt handling and device I/O
    if(mappages(pagetable, 0x10000000, PAGE_SIZE, 0x10000000, PTE_R | PTE_W) < 0) goto bad;  // UART
    /* RTC (0x101000) NOT mapped here — only accessed during kernel boot via kvmmap */
    if(mappages(pagetable, 0x2000000, 0x10000, 0x2000000, PTE_R | PTE_W) < 0) goto bad;      // CLINT
    if(mappages_super(pagetable, 0x0c000000, 0x400000, 0x0c000000, PTE_R | PTE_W) < 0) goto bad; // PLIC
    if(mappages_super(pagetable, 0x30000000, 0x200000, 0x30000000, PTE_R | PTE_W) < 0) goto bad; // PCIe ECAM (first 2MB)
    if(mappages_super(pagetable, 0x40000000, 0x40000000, 0x40000000, PTE_R | PTE_W) < 0) goto bad; // PCIe MMIO BAR

    // Map signal trampoline page
    uvmmap_sigtrampoline(pagetable);

    // 6. Update process state
    current_proc->pagetable = pagetable;
    current_proc->vma_list = new_vmas;
    new_vmas = NULL;
    current_proc->shared_pt = NULL;
    current_proc->shared_sa = NULL;
    current_proc->clone_flags = old_clone_flags & ~(CLONE_VM | CLONE_SIGHAND);
    current_proc->sz = sz;

    // Initialize stack bounds for auto-grow
    current_proc->stack_top = MAXVA;
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

    /* Reset all sigactions to default in the now-private table. */
    memset(current_proc->sigactions, 0, sizeof(current_proc->sigactions));
    /* POSIX: reset signal mask across exec */
    current_proc->sigmask = 0;

    // Switch satp to the new page table immediately so that trap_restore's
    // sret enters user mode with the correct address space.
    w_satp(MAKE_SATP(pagetable));
    sfence_vma();

    /* Defer old-image teardown until sys_execve_handler has rebuilt argv/envp on
     * the new user stack. Freeing the old image inline here has been observed to
     * corrupt post-exec user state on swap-heavy paths. */
    current_proc->deferred_free_pt = old_pt;
    current_proc->deferred_free_vmas = old_vmas;
    current_proc->deferred_shared_pt = old_shared_pt;
    current_proc->deferred_shared_sa = old_shared_sa;

    // 7. Initialize trapframe
    memset(tf, 0, sizeof(*tf));
    tf->epc = ehdr->e_entry;

    // Set up initial stack: crt.S expects [argc, argv..., NULL, envp..., NULL]
    // For no args/env: [0, NULL, NULL] = 24 bytes, padded to 32 for alignment
    uint64_t *stack_top = (uint64_t *)(top_page_mem + PAGE_SIZE);
    stack_top[-1] = 0;  // padding
    stack_top[-2] = 0;  // envp[0] = NULL
    stack_top[-3] = 0;  // argv[0] = NULL
    stack_top[-4] = 0;  // argc = 0
    tf->sp = MAXVA - 32;

    return 0;

bad:
    if (old_satp)
        w_satp(old_satp);
    sfence_vma();
    /* Free user pages from the partially constructed new pagetable.
     * freewalk_user only frees PTE_U pages, leaving kernel identity
     * maps intact (their physical pages are shared with the kernel). */
    if (pagetable) {
        freewalk_user(pagetable);
    }
    if (new_vmas)
        vma_free_list(&new_vmas);
    return -1;
}
