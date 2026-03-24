OUTPUT_ARCH(riscv)
ENTRY(_start)
PHDRS {
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}
SECTIONS {
	. = 0x80200000;
	.text   : { *(.text.init) *(.text) } :text
	/* Trampoline page — page-aligned, symbols for runtime mapping */
	. = ALIGN(4096);
	_trampoline_start = .;
	.text.trampoline : { *(.text.trampoline) } :text
	. = ALIGN(4096);
	_trampoline_end = .;
	.text.rest : { *(.text.*) } :text
	.rodata : { *(.rodata) *(.rodata.*) } :text
	.data   : { *(.data) *(.data.*) } :data
	.tarfs  : { *(.tarfs) } :data
	_bss_start = .;
	.bss    : { *(.bss) *(.bss.*) *(.sbss) *(.sbss.*) *(COMMON) } :data
	. = ALIGN(16);
	. += 16384;
	_stack_top = .;
	_kernel_end = .;
}
