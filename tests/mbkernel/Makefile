CFLAGS = -m32 -O1 -no-pie -static -ffreestanding -fno-pic -fno-stack-protector -nostdlib -mgeneral-regs-only
all: mbtest.elf mb2test.elf
mbtest.elf: entry.S kmain.c link.ld
	gcc $(CFLAGS) -T link.ld -o $@ entry.S kmain.c -Wl,-z,noexecstack -Wl,--build-id=none
mb2test.elf: entry.S kmain.c link.ld
	gcc $(CFLAGS) -DMB2 -T link.ld -o $@ entry.S kmain.c -Wl,-z,noexecstack -Wl,--build-id=none
clean:
	rm -f *.elf
