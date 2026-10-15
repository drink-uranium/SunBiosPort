#!/bin/sh
# Build test media for SunBiosPort in $1 (default build/media):
#   sol10.iso  GRUB legacy menu.lst, kernel$/module$ with $ISADIR (Solaris 10 / OpenSolaris style)
#   sol11.iso  GRUB 2 grub.cfg using $multiboot/$module (Solaris 11 style), Multiboot2 kernel
#   linux.iso  isolinux.cfg booting a Linux bzImage + initrd       (needs LINUX_KERNEL, BUSYBOX)
#   fat.img    MBR + FAT32, syslinux.cfg with mboot.c32 (multiboot via syslinux syntax)
#   ext4.img   MBR + ext4, /boot/grub/grub.cfg booting Linux       (needs LINUX_KERNEL, BUSYBOX)
#   ufs.img    MBR type 0xbf + Solaris x86 VTOC + UFS root slice s0, no menu (Solaris fallback)
#   mbr.img    a disk with only PC boot code (tests the legacy BIOS hand-off)
#   eltorito.iso  a CD with only an El Torito boot image (legacy hand-off)
set -e
out=${1:-build/media}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
t=$(mktemp -d)
make -s -C "$here/mbkernel"
MB=$here/mbkernel/mbtest.elf
MB2=$here/mbkernel/mb2test.elf
head -c 3000000 /dev/urandom > "$t/archive"
printf 'BOOTARCHIVE-TEST' | dd of="$t/archive" conv=notrunc 2>/dev/null

# --- Solaris 10 style ISO ---
r=$t/sol10; mkdir -p $r/boot/grub $r/platform/i86pc/kernel/amd64 $r/platform/i86pc/amd64
cp $MB $r/platform/i86pc/kernel/amd64/unix; cp $MB $r/platform/i86pc/kernel/unix
cp $t/archive $r/platform/i86pc/amd64/boot_archive; cp $t/archive $r/platform/i86pc/boot_archive
cat > $r/boot/grub/menu.lst <<'M'
default=0
timeout=60
splashimage=/boot/grub/splash.xpm.gz
title Solaris Express Developer Edition (test)
	kernel$ /platform/i86pc/kernel/$ISADIR/unix -B install_media=cdrom
	module$ /platform/i86pc/$ISADIR/boot_archive
title Solaris (32-bit)
	kernel /platform/i86pc/kernel/unix -B install_media=cdrom
	module /platform/i86pc/boot_archive
M
genisoimage -quiet -R -J -V SOL_10_TEST -o "$out/sol10.iso" $r

# --- Solaris 11 style ISO (GRUB 2) ---
r=$t/sol11; mkdir -p $r/boot/grub $r/platform/i86pc/kernel/amd64 $r/platform/i86pc/amd64
cp $MB2 $r/platform/i86pc/kernel/amd64/unix; cp $t/archive $r/platform/i86pc/amd64/boot_archive
cat > $r/boot/grub/grub.cfg <<'M'
set default="0"
set timeout="30"
if [ "$grub_platform" = "pc" ]; then
	set multiboot=multiboot2
	set module=module2
fi
menuentry "Oracle Solaris 11.4 Text Installer and command line" {
	insmod part_msdos
	search --no-floppy --file --set=root /.cdrom
	set kern=/platform/i86pc/kernel/amd64/unix
	$multiboot $kern $kern -B install_media=cdrom,console=text
	$module /platform/i86pc/amd64/boot_archive type=rootfs
}
M
touch $r/.cdrom
genisoimage -quiet -R -V SOL_11_TEST -o "$out/sol11.iso" $r

# --- FAT32 disk with syslinux mboot.c32 ---
dd if=/dev/zero of="$out/fat.img" bs=1M count=48 2>/dev/null
printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q "$out/fat.img"
dd if=/dev/zero of=$t/fat.part bs=1M count=47 2>/dev/null
mkfs.fat -F 32 -n SYSLINUX $t/fat.part >/dev/null
mkdir -p $t/fatroot/syslinux
cp $MB $t/fatroot/syslinux/kernel.elf; cp $t/archive $t/fatroot/syslinux/archive.bin
cat > $t/fatroot/syslinux/syslinux.cfg <<'M'
DEFAULT mb
LABEL other
  KERNEL /nothing
LABEL mb
  KERNEL mboot.c32
  APPEND kernel.elf fromsyslinux=1 --- archive.bin modarg=yes
M
mcopy -s -i $t/fat.part $t/fatroot/syslinux ::/
dd if=$t/fat.part of="$out/fat.img" bs=512 seek=2048 conv=notrunc 2>/dev/null

# --- Solaris x86 style disk: fdisk type 0xbf, VTOC in sector 1, UFS slice 0 ---
r=$t/ufsroot; mkdir -p $r/platform/i86pc/kernel/amd64 $r/platform/i86pc/amd64 $r/etc
cp $MB $r/platform/i86pc/kernel/amd64/unix; cp $MB $r/platform/i86pc/kernel/unix
cp $t/archive $r/platform/i86pc/amd64/boot_archive; cp $t/archive $r/platform/i86pc/boot_archive
echo "test" > $r/etc/release
makefs -t ffs -o version=1 -s 40m $t/ufs.part $r >/dev/null
dd if=/dev/zero of="$out/ufs.img" bs=1M count=64 2>/dev/null
printf 'label: dos\nstart=2048, type=bf, bootable\n' | sfdisk -q "$out/ufs.img"
python3 - "$out/ufs.img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
part_start = 2048
vtoc = bytearray(512)
struct.pack_into('<I', vtoc, 12, 0x600DDEEE)      # v_sanity
struct.pack_into('<I', vtoc, 16, 1)               # v_version
struct.pack_into('<HH', vtoc, 28, 512, 16)        # v_sectorsz, v_nparts
def slice_(i, tag, start, size):
    struct.pack_into('<HHII', vtoc, 72 + i * 12, tag, 0, start, size)
slice_(0, 2, 4096, 40 * 2048)      # s0 root at partition-relative sector 4096
slice_(2, 5, 0, 62 * 2048)         # s2 backup
slice_(8, 1, 0, 2048)              # s8 boot
f.seek((part_start + 1) * 512); f.write(vtoc)
PY
dd if=$t/ufs.part of="$out/ufs.img" bs=512 seek=$((2048 + 4096)) conv=notrunc 2>/dev/null

# --- Legacy BIOS boot code: MBR disk and El Torito CD ---
nasm -f bin -DMBR -o $t/mbr.bin "$here/legacy/bootmsg.asm"
nasm -f bin -DELTORITO -o $t/eltorito.bin "$here/legacy/bootmsg.asm"
dd if=/dev/zero of="$out/mbr.img" bs=1M count=16 2>/dev/null
dd if=$t/mbr.bin of="$out/mbr.img" conv=notrunc 2>/dev/null
mkdir -p $t/et/boot; cp $t/eltorito.bin $t/et/boot/
genisoimage -quiet -R -V ELTORITO -b boot/eltorito.bin -no-emul-boot -boot-load-size 4 -o "$out/eltorito.iso" $t/et

# --- Linux media (optional) ---
if [ -n "$LINUX_KERNEL" ] && [ -n "$BUSYBOX" ]; then
	i=$t/initrd; mkdir -p $i/bin $i/proc $i/sys $i/dev
	cp "$BUSYBOX" $i/bin/busybox
	cat > $i/init <<'M'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
echo "LINUXTEST: userspace reached, cmdline: $(/bin/busybox cat /proc/cmdline)"
/bin/busybox poweroff -f
M
	chmod +x $i/init
	(cd $i && find . | cpio -o -H newc 2>/dev/null | gzip -9) > $t/initrd.gz
	r=$t/linuxiso; mkdir -p $r/isolinux
	cp "$LINUX_KERNEL" $r/isolinux/vmlinuz; cp $t/initrd.gz $r/isolinux/initrd.gz
	cat > $r/isolinux/isolinux.cfg <<'M'
DEFAULT live
LABEL live
  MENU LABEL Test Linux
  KERNEL vmlinuz
  APPEND initrd=initrd.gz console=ttyS0 quiet fromisolinux
M
	genisoimage -quiet -R -J -V LINUX_TEST -o "$out/linux.iso" $r
	r=$t/extroot; mkdir -p $r/boot/grub
	cp "$LINUX_KERNEL" $r/boot/vmlinuz; cp $t/initrd.gz $r/boot/initrd.img
	cat > $r/boot/grub/grub.cfg <<'M'
set default=0
menuentry 'Ubuntu' --class ubuntu {
	insmod ext2
	linux	/boot/vmlinuz root=/dev/sda1 ro console=ttyS0 quiet fromgrubcfg
	initrd	/boot/initrd.img
}
M
	dd if=/dev/zero of="$out/ext4.img" bs=1M count=96 2>/dev/null
	printf 'label: dos\nstart=2048, type=83\n' | sfdisk -q "$out/ext4.img"
	mkfs.ext4 -q -F -d $r -E offset=$((2048 * 512)) "$out/ext4.img" 94M
fi
rm -rf $t
ls -la "$out"
