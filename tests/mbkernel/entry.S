/* Test kernel for the SunBiosPort loaders: Multiboot 1 (default) or
 * Multiboot 2 (-DMB2). Reports what it was handed on COM1 and the screen. */
	.section .mbheader, "a"
#ifdef MB2
	.align 8
mb2:	.long 0xe85250d6, 0, mb2_end - mb2, -(0xe85250d6 + (mb2_end - mb2))
	.short 0, 0
	.long 8
mb2_end:
#else
	.align 4
	.long 0x1badb002, 0x00000003, -(0x1badb002 + 3)
#endif
	.text
	.globl _start
_start:
	mov $stack_top, %esp
	push %ebx
	push %eax
	call kmain
1:	hlt
	jmp 1b
	.bss
	.space 16384
stack_top:
	.section .note.GNU-stack,"",@progbits
