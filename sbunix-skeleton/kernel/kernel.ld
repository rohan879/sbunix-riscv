OUTPUT_ARCH(riscv)
ENTRY(_start)
PHDRS {
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}
SECTIONS {
	. = 0x80200000;
	_kernel_start = .;
	_kernel_text_start = .;
	.text   : { *(.text.init) *(.text) *(.text.*) . = ALIGN(0x1000); } :text
	_kernel_text_end = .;
	_kernel_ro_start = .;
	.rodata : {
		*(.rodata) *(.rodata.*)
		*(.srodata) *(.srodata.*)
		. = ALIGN(0x1000);
	} :text
	_kernel_ro_end = .;
	_kernel_rw_start = .;
	.data   : { *(.data) *(.data.*) *(.sdata) *(.sdata.*) } :data
	.tarfs  : { *(.tarfs) } :data
	_bss_start = .;
	.bss (NOLOAD) : { *(.bss) *(.bss.*) *(.sbss) *(.sbss.*) *(COMMON) } :data
	. = ALIGN(0x1000);
	. += 16384;
	_stack_top = .;
	_kernel_end = .;
}
