#include <exec.h>
#include <printk.h>
#include <tar.h>
#include <elf.h>
#include <kalloc.h>
#include <riscv.h>
#include <vm.h>

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
    
    // 1. Find ELF
    elf_tar_header = tar_find_file(_tarfs_start, _tarfs_end, path);
    if (!elf_tar_header) return -1;

    elf_data = (char *)elf_tar_header + sizeof(struct posix_header);
    ehdr = (Elf64_Ehdr *)elf_data;

    // 2. Validate
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) return -1;

    pagetable = uvmcreate();
    if(pagetable == 0) return -1;

    // 3. Load PT_LOAD segments
    phdr = (Elf64_Phdr *)(elf_data + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
        if (phdr->p_type == PT_LOAD) {
            if(uvmalloc(pagetable, phdr->p_vaddr, phdr->p_vaddr + phdr->p_memsz, PTE_W|PTE_X|PTE_R) == 0) return -1;
            if(uvmcopyin(pagetable, phdr->p_vaddr, elf_data + phdr->p_offset, phdr->p_filesz) < 0) return -1;
        }
    }

    // 4. User Stack (at MAXVA - 1 page)
    uint64_t stack_va = MAXVA - PAGE_SIZE;
    char *sp_mem = kalloc();
    if(!sp_mem) return -1;
    if(uvmmap(pagetable, stack_va, (uint64_t)sp_mem, PAGE_SIZE, PTE_R | PTE_W | PTE_U) < 0) return -1;
    
    // 5. Kernel Stack
    char *kstack = kalloc();
    if(!kstack) return -1;

    // 6. Identity map ALL physical RAM into user PT (No PTE_U)
    // This allows kernel code, data, and kstack to work after switching satp.
    if(mappages(pagetable, 0x80000000, PHYSTOP - 0x80000000, 0x80000000, PTE_R | PTE_W | PTE_X) < 0) return -1;
    if(mappages(pagetable, 0x10000000, PAGE_SIZE, 0x10000000, PTE_R | PTE_W) < 0) return -1;

    // 7. Context switch
    w_satp(MAKE_SATP(pagetable));
    sfence_vma();
    asm volatile("fence.i");

    // Configure SSTATUS: SPP=0 (User), SPIE=1, SUM=1
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP;
    x |= SSTATUS_SPIE | SSTATUS_SUM;
    w_sstatus(x);

    asm volatile("csrw sepc, %0" : : "r" (ehdr->e_entry));
    asm volatile("csrw sscratch, %0" : : "r" (kstack + PAGE_SIZE));

    printk("exec: Jumping to user mode at 0x%lx\n", ehdr->e_entry);

    asm volatile(
        "mv sp, %0\n\t"
        "sret"
        : 
        : "r" (MAXVA)
        : "memory"
    );

    return 0;
}
