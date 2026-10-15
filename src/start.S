/*
 * Entry point for the SunBiosPort payload.
 *
 * The same ELF can be started three ways:
 *   - as a coreboot payload: 32-bit flat protected mode, entry called like a
 *     C function with the coreboot table pointer as its argument;
 *   - by a Multiboot loader (GRUB, QEMU -kernel): EAX = 0x2BADB002, EBX = MBI;
 *   - by anything else that jumps to the ELF entry in flat 32-bit mode.
 */
	.set MB_MAGIC, 0x1BADB002
	.set MB_FLAGS, 0x00000003	/* page-align modules, want memory info */

	.section .multiboot, "a"
	.align 4
	.long MB_MAGIC
	.long MB_FLAGS
	.long -(MB_MAGIC + MB_FLAGS)

	.section .text.entry, "ax"
	.code32
	.globl _start
_start:
	cli
	cld
	mov	%eax, boot_magic
	mov	%ebx, boot_mbi
	mov	4(%esp), %ecx
	mov	%ecx, boot_arg

	lgdt	gdt_desc
	ljmp	$0x10, $1f
1:	mov	$0x18, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	xor	%ax, %ax
	mov	%ax, %fs
	mov	%ax, %gs
	mov	$stack_top, %esp
	xor	%ebp, %ebp

	/* Zero .bss in case the loader did not. */
	mov	$__bss_start, %edi
	mov	$__bss_end, %ecx
	sub	%edi, %ecx
	xor	%eax, %eax
	rep	stosb

	/* Enable SSE so the compiler's occasional use of it is harmless. */
	mov	%cr0, %eax
	and	$~0x4, %eax
	or	$0x2, %eax
	mov	%eax, %cr0
	mov	%cr4, %eax
	or	$0x600, %eax
	mov	%eax, %cr4
	fninit

	push	boot_arg
	push	boot_mbi
	push	boot_magic
	call	c_main
2:	hlt
	jmp	2b

/* int setjmp(jmp_buf) / void longjmp(jmp_buf, int) */
	.globl setjmp
setjmp:
	mov	4(%esp), %eax
	mov	%ebx, 0(%eax)
	mov	%esi, 4(%eax)
	mov	%edi, 8(%eax)
	mov	%ebp, 12(%eax)
	lea	4(%esp), %ecx
	mov	%ecx, 16(%eax)
	mov	(%esp), %ecx
	mov	%ecx, 20(%eax)
	xor	%eax, %eax
	ret

	.globl longjmp
longjmp:
	mov	4(%esp), %edx
	mov	8(%esp), %eax
	test	%eax, %eax
	jnz	1f
	inc	%eax
1:	mov	0(%edx), %ebx
	mov	4(%edx), %esi
	mov	8(%edx), %edi
	mov	12(%edx), %ebp
	mov	16(%edx), %esp
	jmp	*20(%edx)

/*
 * void boot_jump(u32 entry, u32 eax, u32 ebx, u32 esi)
 * Final hand-off to an operating system: flat segments (CS=0x10, DS=0x18 as
 * the Linux 32-bit boot protocol requires), interrupts off, paging off.
 */
	.globl boot_jump
boot_jump:
	cli
	mov	4(%esp), %ecx
	mov	8(%esp), %eax
	mov	12(%esp), %ebx
	mov	16(%esp), %esi
	xor	%edi, %edi
	xor	%ebp, %ebp
	mov	$0x7000, %esp		/* a stack the OS will not mind losing */
	push	%ecx
	xor	%ecx, %ecx
	xor	%edx, %edx
	ret

/* void call_payload(u32 entry, u32 arg): start a coreboot payload (SeaBIOS) */
	.globl call_payload
call_payload:
	cli
	mov	4(%esp), %ecx
	mov	8(%esp), %eax
	mov	$0x7000, %esp
	push	%eax
	push	$0
	jmp	*%ecx

	.section .rodata
	.align 16
	.globl gdt
gdt:
	.quad 0x0000000000000000	/* 0x00 null */
	.quad 0x00cf9a000000ffff	/* 0x08 code32 flat */
	.quad 0x00cf9a000000ffff	/* 0x10 code32 flat (__BOOT_CS) */
	.quad 0x00cf92000000ffff	/* 0x18 data32 flat (__BOOT_DS) */
	.quad 0x00009a000000ffff	/* 0x20 code16, base 0, 64K */
	.quad 0x000092000000ffff	/* 0x28 data16, base 0, 64K */
gdt_end:
	.globl gdt_desc
gdt_desc:
	.word gdt_end - gdt - 1
	.long gdt

	.globl realmode_blob, realmode_blob_end
	.align 16
realmode_blob:
	.incbin "build/realmode.bin"
realmode_blob_end:

	.section .data
	.align 4
	.globl boot_magic, boot_mbi, boot_arg
boot_magic:	.long 0
boot_mbi:	.long 0
boot_arg:	.long 0

	.section .bss
	.align 4096
stack_bottom:
	.space 0x20000
stack_top:

	.section .note.GNU-stack,"",@progbits
