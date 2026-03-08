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
            if(uvmalloc(pagetable, phdr->p_vaddr, phdr->p_vaddr + phdr->p_memsz, PTE_R|PTE_W|PTE_X) == 0) return -1;
            if(copyout(pagetable, phdr->p_vaddr, elf_data + phdr->p_offset, phdr->p_filesz) < 0) return -1;
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

    // 4. User Stack — allocate 8 pages (32 KB) to accommodate deep call stacks
    #define USER_STACK_PAGES 8
    uint64_t stack_va = MAXVA - (USER_STACK_PAGES * PAGE_SIZE);
    char *top_page_mem = NULL;
    for (int pg = 0; pg < USER_STACK_PAGES; pg++) {
        char *sp_mem = kalloc();
        if (!sp_mem) return -1;
        uint64_t vaddr = stack_va + pg * PAGE_SIZE;
        if (uvmmap(pagetable, vaddr, (uint64_t)sp_mem, PAGE_SIZE, PTE_R | PTE_W | PTE_U) < 0) return -1;
        if (pg == USER_STACK_PAGES - 1)
            top_page_mem = sp_mem;  /* remember physical addr of topmost page */
    }
    
    /* Add a VMA for the user stack region */
    vma_add(current_proc, stack_va, MAXVA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON);

    // 5. Identity map kernel and ALL RAM (No PTE_U)
    // This ensures kernel code, data, AND dynamically allocated pages (like kstack)
    // are accessible after switching satp.
    if(mappages_super(pagetable, 0x80000000, PHYSTOP - 0x80000000, 0x80000000, PTE_R | PTE_W | PTE_X) < 0) return -1;
    // Map I/O registers needed for interrupt handling and device I/O
    if(mappages(pagetable, 0x10000000, PAGE_SIZE, 0x10000000, PTE_R | PTE_W) < 0) return -1;  // UART
    if(mappages(pagetable, 0x101000, PAGE_SIZE, 0x101000, PTE_R | PTE_W) < 0) return -1;    // RTC
    if(mappages(pagetable, 0x2000000, 0x10000, 0x2000000, PTE_R | PTE_W) < 0) return -1;      // CLINT
    if(mappages_super(pagetable, 0x0c000000, 0x400000, 0x0c000000, PTE_R | PTE_W) < 0) return -1; // PLIC
    if(mappages_super(pagetable, 0x30000000, 0x200000, 0x30000000, PTE_R | PTE_W) < 0) return -1; // PCIe ECAM (first 2MB)
    if(mappages_super(pagetable, 0x40000000, 0x40000000, 0x40000000, PTE_R | PTE_W) < 0) return -1; // PCIe MMIO BAR

    // Map signal trampoline page
    uvmmap_sigtrampoline(pagetable);

    // 6. Update process state
    // TODO: cleanup old pagetable
    current_proc->pagetable = pagetable;
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

    // Switch satp to the new page table immediately so that trap_restore's
    // sret enters user mode with the correct address space.
    w_satp(MAKE_SATP(pagetable));
    sfence_vma();

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
}