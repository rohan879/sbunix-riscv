#ifndef _ELF_H
#define _ELF_H

#include <riscv.h>

// Common ELF data types
typedef uint64_t Elf64_Addr;   // Unsigned program address
typedef uint64_t Elf64_Off;    // Unsigned file offset
typedef uint16_t Elf64_Half;   // Unsigned 16-bit quantity
typedef uint32_t Elf64_Word;   // Unsigned 32-bit quantity
typedef int32_t  Elf64_Sword;  // Signed 32-bit quantity
typedef uint64_t Elf64_Xword;  // Unsigned 64-bit quantity
typedef int64_t  Elf64_Sxword; // Signed 64-bit quantity

#define EI_NIDENT 16

// ELF Header Structure
typedef struct {
    unsigned char e_ident[EI_NIDENT]; /* ELF identification bytes */
    Elf64_Half    e_type;             /* Object file type */
    Elf64_Half    e_machine;          /* Machine architecture */
    Elf64_Word    e_version;          /* ELF format version */
    Elf64_Addr    e_entry;            /* Entry point virtual address */
    Elf64_Off     e_phoff;            /* Program header table file offset */
    Elf64_Off     e_shoff;            /* Section header table file offset */
    Elf64_Word    e_flags;            /* Processor-specific flags */
    Elf64_Half    e_ehsize;           /* ELF header size in bytes */
    Elf64_Half    e_phentsize;        /* Size of program header table entry */
    Elf64_Half    e_phnum;            /* Number of program header table entries */
    Elf64_Half    e_shentsize;        /* Size of section header table entry */
    Elf64_Half    e_shnum;            /* Number of section header table entries */
    Elf64_Half    e_shstrndx;         /* Section header string table index */
} Elf64_Ehdr;

// Program Header Structure
typedef struct {
    Elf64_Word p_type;    /* Type of segment */
    Elf64_Word p_flags;   /* Segment flags */
    Elf64_Off  p_offset;  /* Offset of segment in file */
    Elf64_Addr p_vaddr;   /* Virtual address in memory */
    Elf64_Addr p_paddr;   /* Physical address (ignored for most systems) */
    Elf64_Xword p_filesz; /* Size of segment in file */
    Elf64_Xword p_memsz;  /* Size of segment in memory */
    Elf64_Xword p_align;  /* Alignment of segment */
} Elf64_Phdr;

// ELF magic number bytes
#define ELFMAG0 0x7f // e_ident[EI_MAG0]
#define ELFMAG1 'E'  // e_ident[EI_MAG1]
#define ELFMAG2 'L'  // e_ident[EI_MAG2]
#define ELFMAG3 'F'  // e_ident[EI_MAG3]

// Program segment types
#define PT_LOAD 1  // Loadable program segment

// Program segment flags
#define PF_X 0x1 // Executable
#define PF_W 0x2 // Writable
#define PF_R 0x4 // Readable


#endif