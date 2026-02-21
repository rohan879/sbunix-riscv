#include <exec.h>
#include <printk.h>
#include <tar.h>
#include <elf.h>
#include <kalloc.h>
#include <riscv.h>
#include <vm.h>
#include <proc.h>

// --- Static implementations of standard library functions ---
static void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

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

    pagetable = uvmcreate();
    if(pagetable == 0) return -1;

    // 3. Load PT_LOAD segments
    for (i = 0; i < ehdr->e_phnum; i++) {
        phdr = (Elf64_Phdr *)(elf_data + ehdr->e_phoff + i*sizeof(Elf64_Phdr));
        if (phdr->p_type == PT_LOAD) {
            if(uvmalloc(pagetable, phdr->p_vaddr, phdr->p_vaddr + phdr->p_memsz, PTE_W|PTE_X|PTE_R) == 0) return -1;
            if(uvmcopyin(pagetable, phdr->p_vaddr, elf_data + phdr->p_offset, phdr->p_filesz) < 0) return -1;
        }
    }

    // 4. User Stack
    uint64_t stack_va = MAXVA - PAGE_SIZE;
    char *sp_mem = kalloc();
    if(!sp_mem) return -1;
    if(uvmmap(pagetable, stack_va, (uint64_t)sp_mem, PAGE_SIZE, PTE_R | PTE_W | PTE_U) < 0) return -1;
    
    // 5. Identity map kernel and ALL RAM (No PTE_U)
    // This ensures kernel code, data, AND dynamically allocated pages (like kstack)
    // are accessible after switching satp.
    if(mappages(pagetable, 0x80000000, PHYSTOP - 0x80000000, 0x80000000, PTE_R | PTE_W | PTE_X) < 0) return -1;
    if(mappages(pagetable, 0x10000000, PAGE_SIZE, 0x10000000, PTE_R | PTE_W) < 0) return -1;

    // 6. Update process state
    // TODO: cleanup old pagetable
    current_proc->pagetable = pagetable;
    current_proc->sz = sz;

    // 7. Initialize trapframe
    memset(tf, 0, sizeof(*tf));
    tf->epc = ehdr->e_entry;
    tf->sp = MAXVA;

    return 0; 
}