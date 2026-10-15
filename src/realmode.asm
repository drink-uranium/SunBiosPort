; Real-mode hand-off stub. Copied to 0x6000 and entered from 32-bit
; protected mode (CS=0x10) when a legacy PC BIOS is still resident below 1MB,
; e.g. when SunBiosPort was started by GRUB or QEMU instead of coreboot.
;
; Parameter block at 0x5E00:
;   +0  dword  mode   1 = load sector 0 of BIOS drive to 0x7C00 and jump
;                     2 = INT 19h (let the BIOS bootstrap loader decide)
;   +4  byte   drive  BIOS drive number for mode 1
	org 0x6000
	bits 32
PARAM	equ 0x5E00
DAP	equ 0x5E10

	cli
	lgdt [gdt16_desc]
	jmp 0x20:pm16
	bits 16
pm16:
	mov ax, 0x28
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	mov eax, cr0
	and eax, 0x7FFFFFFE
	mov cr0, eax
	jmp 0:rm
rm:
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	mov sp, 0x7C00
	lidt [rm_idt]
	sti
	mov ax, 0x0003		; back to 80x25 text mode
	int 0x10

	cmp dword [PARAM], 2
	je .int19

	mov dl, [PARAM + 4]
	mov si, DAP
	mov byte [si], 16
	mov byte [si + 1], 0
	mov word [si + 2], 1
	mov word [si + 4], 0x7C00
	mov word [si + 6], 0
	mov dword [si + 8], 0
	mov dword [si + 12], 0
	mov ah, 0x42
	int 0x13
	jnc .loaded
	; fall back to CHS read of cylinder 0, head 0, sector 1
	mov dl, [PARAM + 4]
	mov ax, 0x0201
	mov cx, 0x0001
	xor dh, dh
	mov bx, 0x7C00
	int 0x13
	jc .int19
.loaded:
	cmp word [0x7DFE], 0xAA55
	jne .int19
	mov dl, [PARAM + 4]
	xor ax, ax
	xor bx, bx
	xor cx, cx
	xor si, si
	xor di, di
	xor bp, bp
	jmp 0:0x7C00
.int19:
	int 0x19
.hang:
	hlt
	jmp .hang

	align 8
gdt16:
	dq 0
	dq 0x00cf9a000000ffff
	dq 0x00cf9a000000ffff
	dq 0x00cf92000000ffff
	dq 0x00009a000000ffff
	dq 0x000092000000ffff
gdt16_desc:
	dw gdt16_desc - gdt16 - 1
	dd gdt16
rm_idt:
	dw 0x3FF
	dd 0
