/*
 * Display bring-up when coreboot did not hand us a framebuffer: program
 * the Bochs/QEMU "DISPI" interface directly (1024x768x32, the resolution
 * of a SPARCstation console). The VGA text-mode state left by a BIOS is
 * saved first and restored before handing over to an OS that expects it.
 */
#include "boot.h"
#include "pci.h"
#include "console.h"

static bool saved;
static u8 s_misc, s_seq[5], s_crtc[25], s_gc[9], s_ac[21], s_dac[768];
static u8 *s_planes;	/* 3 x 64 KiB */
static u16 dispi_port_idx = 0x1ce;
static uintptr_t dispi_mmio;

static void dispi_w(u16 idx, u16 v)
{
	if (dispi_mmio) { write16(dispi_mmio + 0x500 + idx * 2, v); return; }
	outw(dispi_port_idx, idx);
	outw(dispi_port_idx + 1, v);
}

static u16 dispi_r(u16 idx)
{
	if (dispi_mmio) return read16(dispi_mmio + 0x500 + idx * 2);
	outw(dispi_port_idx, idx);
	return inw(dispi_port_idx + 1);
}

static u8 ac_read(u8 i) { inb(0x3da); outb(0x3c0, i); return inb(0x3c1); }
static void ac_write(u8 i, u8 v) { inb(0x3da); outb(0x3c0, i); outb(0x3c0, v); }

void vga_save_text(void)
{
	s_misc = inb(0x3cc);
	if (!(s_misc & 1)) return;	/* only colour text mode at 0x3d4 */
	outb(0x3ce, 6);
	if (inb(0x3cf) & 1) return;	/* already graphics */
	for (int i = 0; i < 5; i++) { outb(0x3c4, i); s_seq[i] = inb(0x3c5); }
	for (int i = 0; i < 25; i++) { outb(0x3d4, i); s_crtc[i] = inb(0x3d5); }
	for (int i = 0; i < 9; i++) { outb(0x3ce, i); s_gc[i] = inb(0x3cf); }
	for (int i = 0; i < 21; i++) s_ac[i] = ac_read(i);
	inb(0x3da); outb(0x3c0, 0x20);
	outb(0x3c7, 0);
	for (int i = 0; i < 768; i++) s_dac[i] = inb(0x3c9);
	s_planes = malloc(3 * 65536);
	if (!s_planes) return;
	/* planar read access */
	outb(0x3c4, 4); outb(0x3c5, 0x06);
	outb(0x3ce, 5); outb(0x3cf, 0x00);
	outb(0x3ce, 6); outb(0x3cf, 0x05);
	for (int p = 0; p < 3; p++) {
		outb(0x3ce, 4); outb(0x3cf, p);
		memcpy(s_planes + p * 65536, (void *)0xa0000, 65536);
	}
	/* put the registers back as they were */
	outb(0x3c4, 4); outb(0x3c5, s_seq[4]);
	for (int i = 4; i < 7; i++) { outb(0x3ce, i); outb(0x3cf, s_gc[i]); }
	saved = true;
}

bool vga_text_saved(void) { return saved; }

bool vga_restore_text(void)
{
	if (!saved) return false;
	dispi_w(4, 0);		/* DISPI off: back to VGA */
	/* planar write access */
	outb(0x3c4, 0); outb(0x3c5, 0x01);
	outb(0x3c4, 4); outb(0x3c5, 0x06);
	outb(0x3c4, 0); outb(0x3c5, 0x03);
	outb(0x3ce, 1); outb(0x3cf, 0);
	outb(0x3ce, 3); outb(0x3cf, 0);
	outb(0x3ce, 5); outb(0x3cf, 0);
	outb(0x3ce, 6); outb(0x3cf, 0x05);
	outb(0x3ce, 8); outb(0x3cf, 0xff);
	for (int p = 0; p < 3; p++) {
		outb(0x3c4, 2); outb(0x3c5, 1 << p);
		memcpy((void *)0xa0000, s_planes + p * 65536, 65536);
	}
	/* registers */
	outb(0x3c2, s_misc);
	outb(0x3c4, 0); outb(0x3c5, 0x01);
	for (int i = 1; i < 5; i++) { outb(0x3c4, i); outb(0x3c5, s_seq[i]); }
	outb(0x3c4, 0); outb(0x3c5, 0x03);
	outb(0x3d4, 0x11); outb(0x3d5, s_crtc[0x11] & 0x7f);
	for (int i = 0; i < 25; i++) { outb(0x3d4, i); outb(0x3d5, i == 0x11 ? s_crtc[i] & 0x7f : s_crtc[i]); }
	outb(0x3d4, 0x11); outb(0x3d5, s_crtc[0x11]);
	for (int i = 0; i < 9; i++) { outb(0x3ce, i); outb(0x3cf, s_gc[i]); }
	for (int i = 0; i < 21; i++) ac_write(i, s_ac[i]);
	inb(0x3da); outb(0x3c0, 0x20);
	outb(0x3c8, 0);
	for (int i = 0; i < 768; i++) outb(0x3c9, s_dac[i]);
	/* clear the screen, home the cursor */
	for (int i = 0; i < 80 * 25; i++) ((volatile u16 *)0xb8000)[i] = 0x0720;
	outb(0x3d4, 0x0e); outb(0x3d5, 0);
	outb(0x3d4, 0x0f); outb(0x3d5, 0);
	return true;
}

bool video_init_bochs(struct fb_info *fb)
{
	struct pci_dev *dev = NULL;
	for (int i = 0; i < pci_ndevs; i++) {
		struct pci_dev *p = &pci_devs[i];
		if (p->vendor == 0x1234 && p->device == 0x1111) { dev = p; break; }
		if (p->vendor == 0x15ad && p->device == 0x0405) { dev = p; break; }	/* VMware SVGA II has DISPI-compatible ports */
	}
	if (!dev || dev->vendor != 0x1234) return false;
	pci_enable(dev);
	if (dev->class == 3 && dev->subclass == 0x80) dispi_mmio = pci_bar_mem(dev, 2);
	u16 id = dispi_r(0);
	if (id < 0xb0c0 || id > 0xb0cf) return false;
	uintptr_t lfb = pci_bar_mem(dev, 0);
	if (!lfb) return false;
	if (dev->class == 3 && dev->subclass == 0) vga_save_text();
	dispi_w(4, 0);
	dispi_w(1, 1024);
	dispi_w(2, 768);
	dispi_w(3, 32);
	dispi_w(6, 1024);
	dispi_w(7, 768);
	dispi_w(8, 0);
	dispi_w(9, 0);
	dispi_w(4, 0x41);
	if (dispi_r(1) != 1024) return false;
	memset(fb, 0, sizeof(*fb));
	fb->base = lfb;
	fb->width = 1024;
	fb->height = 768;
	fb->bpp = 32;
	fb->pitch = 1024 * 4;
	fb->rpos = 16; fb->rsize = 8;
	fb->gpos = 8; fb->gsize = 8;
	fb->bpos = 0; fb->bsize = 8;
	return true;
}
