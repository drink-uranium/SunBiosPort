#!/bin/sh
# Boot SunBiosPort in QEMU against the media from tests/mkmedia.sh and check
# what the loaded kernel reports on the serial port.
#   tests/run-tests.sh                      (Multiboot via qemu -kernel)
#   COREBOOT_ROM=coreboot.rom tests/run-tests.sh   (also as a coreboot payload)
cd "$(dirname "$0")/.." || exit 1
M=build/media
[ -f $M/sol10.iso ] || tests/mkmedia.sh $M >/dev/null
out=$(mktemp -d)
pass=0; fail=0
run() { # name expect seconds qemu-args...
	name=$1; expect=$2; secs=$3; shift 3
	log=$out/$name.log
	timeout $secs qemu-system-x86_64 -display none -m 512 -serial file:$log -no-reboot "$@" >/dev/null 2>&1
	if grep -aq -- "$expect" $log; then echo "PASS  $name"; pass=$((pass + 1))
	else echo "FAIL  $name (expected: $expect, log: $log)"; fail=$((fail + 1)); fi
}
K="-kernel build/sunbios.elf"
run sol10-menu.lst   "cmdline '/platform/i86pc/kernel/amd64/unix -B install_media=cdrom'" 15 $K -cdrom $M/sol10.iso
run sol10-module     "module 0 at .* '/platform/i86pc/amd64/boot_archive' data 'BOOTARCHIVE-TEST" 15 $K -cdrom $M/sol10.iso
run sol11-grub2-mb2  "mb2 cmdline '/platform/i86pc/kernel/amd64/unix -B install_media=cdrom,console=text'" 15 $K -cdrom $M/sol11.iso
run solaris-vtoc-ufs "cmdline '/platform/i86pc/kernel/amd64/unix'" 15 $K -drive file=$M/ufs.img,format=raw
run syslinux-mboot   "module 0 at .* '/syslinux/archive.bin modarg=yes'" 15 $K -drive file=$M/fat.img,format=raw
run ahci-q35         "cmdline '/platform/i86pc/kernel/amd64/unix'" 15 -M q35 $K -drive file=$M/ufs.img,format=raw,if=none,id=d0 -device ide-hd,drive=d0,bus=ide.0
run nvme             "cmdline '/syslinux/kernel.elf fromsyslinux=1'" 15 $K -drive file=$M/fat.img,format=raw,if=none,id=n0 -device nvme,drive=n0,serial=1
run bios-mbr-chain   "LEGACY-MBR-OK" 15 $K -drive file=$M/mbr.img,format=raw
if [ -f $M/linux.iso ]; then
	run linux-isolinux "LINUXTEST: userspace reached" 60 $K -cdrom $M/linux.iso
	run linux-grub2-ext4 "LINUXTEST: userspace reached" 60 $K -drive file=$M/ext4.img,format=raw
fi
if [ -n "$COREBOOT_ROM" ]; then
	C="-bios $COREBOOT_ROM"
	run cb-sol10      "module 0 at .* '/platform/i86pc/amd64/boot_archive'" 25 $C -cdrom $M/sol10.iso
	run cb-sol11      "mb2 module size 3000000" 25 $C -cdrom $M/sol11.iso
	run cb-seabios-mbr "LEGACY-MBR-OK" 30 $C -drive file=$M/mbr.img,format=raw
	run cb-seabios-cd  "LEGACY-ELTORITO-OK" 30 $C -cdrom $M/eltorito.iso
	[ -f $M/linux.iso ] && run cb-linux "LINUXTEST: userspace reached" 60 $C -cdrom $M/linux.iso
fi
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
