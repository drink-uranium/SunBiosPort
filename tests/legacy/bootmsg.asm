; Minimal PC boot code for testing the legacy hand-off: prints a message
; through INT 10h and straight to COM1, then halts.
;   nasm -f bin -DMBR      -> 512-byte MBR (0x55AA)
;   nasm -f bin -DELTORITO -> 2048-byte no-emulation El Torito image
	bits 16
	org 0x7c00
start:
	cli
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x7c00
	sti
	mov si, msg
.next:
	lodsb
	or al, al
	jz .done
	mov bl, al
	mov ah, 0x0e
	mov bh, 0
	int 0x10
	mov dx, 0x3fd
.wait:
	in al, dx
	test al, 0x20
	jz .wait
	mov dx, 0x3f8
	mov al, bl
	out dx, al
	jmp .next
.done:
	cli
	hlt
	jmp .done
%ifdef MBR
msg:	db "LEGACY-MBR-OK: booted by the PC BIOS", 13, 10, 0
	times 446-($-$$) db 0
	db 0x80, 0, 1, 0, 0x0c, 0xfe, 0xff, 0xff
	dd 2048, 20480
	times 510-($-$$) db 0
	dw 0xaa55
%else
msg:	db "LEGACY-ELTORITO-OK: booted by the PC BIOS", 13, 10, 0
	times 2048-($-$$) db 0
%endif
