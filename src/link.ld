/* SunBiosPort payload: a single 32-bit ELF linked at 32 MiB, clear of the
 * 1 MiB region where Multiboot and Linux kernels are loaded. */
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
	. = 0x02000000;
	__image_start = .;
	.text : {
		KEEP(*(.multiboot))
		*(.text.entry)
		*(.text .text.*)
	}
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	.bss : {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		__bss_end = .;
	}
	__image_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
