# SunBiosPort: OpenBoot for x86 as a coreboot payload.
#
#   make                 build build/sunbios.elf
#   make NVRAM_CMOS=1    keep NVRAM settings in CMOS bank 1 (0x72/0x73)
#   make OBP_REV=3.11    change the "ROM Rev." shown in the banner (default 2.15)
#   make run             boot it in QEMU (Multiboot via -kernel)

CC      ?= gcc
LD      ?= ld
NASM    ?= nasm
OBJCOPY ?= objcopy
QEMU    ?= qemu-system-x86_64

CFLAGS := -m32 -march=i686 -O2 -g -std=gnu11 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin \
	-fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -fno-strict-aliasing -mgeneral-regs-only -nostdinc \
	-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers --param=min-pagesize=0 -Isrc
ifeq ($(NVRAM_CMOS),1)
CFLAGS += -DNVRAM_CMOS
endif
ifdef OBP_REV
CFLAGS += -DOBP_REV=\"$(OBP_REV)\"
endif
LIBGCC := $(shell $(CC) -m32 -print-libgcc-file-name)

SRCS := main.c libc.c console.c fb.c gallant.c sunlogo.c platform.c timer.c pci.c \
	blockdev.c ata.c ahci.c nvme.c fs.c iso9660.c fat.c ext2.c ufs.c \
	boot.c loader.c vga.c cbfs.c lzma.c net.c nvram.c forth.c obp.c devtree.c
OBJS := build/start.o $(SRCS:%.c=build/%.o)

all: build/sunbios.elf

build:
	mkdir -p build

build/realmode.bin: src/realmode.asm | build
	$(NASM) -f bin -o $@ $<

build/start.o: src/start.S build/realmode.bin | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

src/gallant.c src/sunlogo.c: third_party/openboot/gallant.fth third_party/openboot/logodata.fth tools/fth2c.py
	python3 tools/fth2c.py third_party/openboot/gallant.fth third_party/openboot/logodata.fth src/gallant.c src/sunlogo.c

build/sunbios.elf: $(OBJS) src/link.ld
	$(LD) -m elf_i386 -z noexecstack --no-warn-rwx-segments -T src/link.ld -nostdlib -o build/sunbios-debug.elf $(OBJS) $(LIBGCC)
	$(OBJCOPY) --strip-debug build/sunbios-debug.elf $@
	@echo "Built $@"

run: build/sunbios.elf
	$(QEMU) -m 512 -kernel build/sunbios.elf -serial stdio

test: build/sunbios.elf
	tests/run-tests.sh

dist: build/sunbios.elf
	mkdir -p dist && cp build/sunbios.elf dist/sunbios.elf

clean:
	rm -rf build

.PHONY: all run test dist clean
