/*
 * Operating system image loaders: Multiboot (v1 and v2), Linux bzImage
 * (32-bit boot protocol) and plain ELF / coreboot-style payloads.
 *
 * x86 Solaris 10/11 and illumos kernels are Multiboot kernels
 * (/platform/i86pc/kernel/[amd64/]unix, or /boot/multiboot on early
 * Solaris 10), loaded together with the boot archive as a module.
 */
#include "boot.h"
#include "platform.h"
#include "console.h"

/* ---------------- physical memory placement ---------------- */
static struct { u64 s, e; } resv[64];
static int nresv;

void phys_reserve(u64 s, u64 e)
{
	if (nresv < 64 && e > s) { resv[nresv].s = s; resv[nresv].e = e; nresv++; }
}

void phys_reset(void)
{
	nresv = 0;
	uintptr_t hs, he;
	heap_bounds(&hs, &he);
	phys_reserve(0, 0x10000);			/* IVT, BDA, hand-off stack and stubs */
	u32 ebda = (u32)read16(0x40e) << 4;
	if (ebda < 0x80000 || ebda >= 0xa0000) ebda = 0x9f000;
	phys_reserve(ebda & ~0xfffu, 0x100000);	/* EBDA, VGA, ROMs */
	phys_reserve((uintptr_t)__image_start, MAX((uintptr_t)__image_end, he));
}

bool phys_free_range(u64 s, u64 e)
{
	for (int i = 0; i < nresv; i++)
		if (s < resv[i].e && e > resv[i].s) return false;
	return true;
}

u64 phys_alloc(u64 size, u64 align, u64 min, u64 max, bool high)
{
	if (!align) align = 4096;
	if (max > 0xfffff000ull) max = 0xfffff000ull;
	u64 best = 0;
	bool found = false;
	for (int i = 0; i < sysinfo.nmem; i++) {
		struct memrange *m = &sysinfo.mem[i];
		if (m->type != MEM_RAM) continue;
		u64 lo = MAX(m->base, min), hi = MIN(m->base + m->size, max);
		if (hi <= lo || hi - lo < size) continue;
		if (!high) {
			u64 a = ALIGN_UP(lo, align);
			for (int guard = 0; guard < 200 && a + size <= hi; guard++) {
				int clash = -1;
				for (int r = 0; r < nresv; r++)
					if (a < resv[r].e && a + size > resv[r].s) { clash = r; break; }
				if (clash < 0) {
					if (!found || a < best) { best = a; found = true; }
					break;
				}
				a = ALIGN_UP(resv[clash].e, align);
			}
		} else {
			u64 a = (hi - size) & ~(align - 1);
			for (int guard = 0; guard < 200 && a >= lo; guard++) {
				int clash = -1;
				for (int r = 0; r < nresv; r++)
					if (a < resv[r].e && a + size > resv[r].s) { clash = r; break; }
				if (clash < 0) {
					if (!found || a > best) { best = a; found = true; }
					break;
				}
				if (resv[clash].s < size) break;
				a = (resv[clash].s - size) & ~(align - 1);
			}
		}
	}
	if (!found) return 0;
	phys_reserve(best, best + size);
	return best;
}

int src_read(struct src *s, u64 off, u32 len, void *buf)
{
	if (off + len > s->size) return -1;
	if (s->mem) { memcpy(buf, s->mem + off, len); return 0; }
	return fs_read(s->file, off, len, buf) == (int)len ? 0 : -1;
}

/* Read a whole source into physical memory with a progress spinner. */
static int src_load(struct src *s, u64 off, u32 len, u64 dest)
{
	static const char spin[] = "|/-\\";
	u32 done = 0, chunk = 4 * 1024 * 1024;
	int k = 0;
	bool show = len > 8 * 1024 * 1024;
	while (done < len) {
		u32 n = MIN(chunk, len - done);
		if (src_read(s, off + done, n, (void *)(uintptr_t)(dest + done)) < 0) return -1;
		done += n;
		if (show) { console_putc(spin[k++ & 3]); console_putc('\b'); console_flush(); }
	}
	return 0;
}

static int place_check(u64 s, u64 e)
{
	if (e > 0xffffffffull || !mem_is_ram(s, e - s)) {
		printf("Image range %llx-%llx is not in usable RAM\n", s, e);
		return -1;
	}
	if (!phys_free_range(s, e)) {
		printf("Image range %llx-%llx overlaps firmware or reserved memory\n", s, e);
		return -1;
	}
	phys_reserve(s, e);
	return 0;
}

/* ---------------- ELF ---------------- */
struct elf_seg { u64 paddr, vaddr, filesz, memsz, off; };

static int elf_load(struct src *k, u32 *entry_out, u64 *lo_out, u64 *hi_out)
{
	u8 eh[64];
	if (src_read(k, 0, 64, eh) < 0 || memcmp(eh, "\177ELF", 4)) return -1;
	bool is64 = eh[4] == 2;
	u64 entry = is64 ? *(u64 *)(eh + 24) : *(u32 *)(eh + 24);
	u64 phoff = is64 ? *(u64 *)(eh + 32) : *(u32 *)(eh + 28);
	u16 phentsize = *(u16 *)(eh + (is64 ? 54 : 42));
	u16 phnum = *(u16 *)(eh + (is64 ? 56 : 44));
	if (phnum > 64) return -1;
	struct elf_seg segs[64];
	int nseg = 0;
	u64 lo = ~0ull, hi = 0;
	for (int i = 0; i < phnum; i++) {
		u8 ph[56];
		if (src_read(k, phoff + (u64)i * phentsize, is64 ? 56 : 32, ph) < 0) return -1;
		if (*(u32 *)ph != 1) continue;	/* PT_LOAD */
		struct elf_seg *s = &segs[nseg++];
		if (is64) {
			s->off = *(u64 *)(ph + 8); s->vaddr = *(u64 *)(ph + 16); s->paddr = *(u64 *)(ph + 24);
			s->filesz = *(u64 *)(ph + 32); s->memsz = *(u64 *)(ph + 40);
		} else {
			s->off = *(u32 *)(ph + 4); s->vaddr = *(u32 *)(ph + 8); s->paddr = *(u32 *)(ph + 12);
			s->filesz = *(u32 *)(ph + 16); s->memsz = *(u32 *)(ph + 20);
		}
		if (!s->memsz) { nseg--; continue; }
		if (s->paddr + s->memsz > 0x100000000ull) {
			printf("Segment at %llx cannot be loaded below 4GB\n", s->paddr);
			return -2;
		}
		if (place_check(s->paddr, s->paddr + s->memsz) < 0) return -2;
		lo = MIN(lo, s->paddr); hi = MAX(hi, s->paddr + s->memsz);
	}
	if (!nseg) return -1;
	for (int i = 0; i < nseg; i++) {
		struct elf_seg *s = &segs[i];
		if (s->filesz && src_load(k, s->off, s->filesz, s->paddr) < 0) return -2;
		if (s->memsz > s->filesz)
			memset((void *)(uintptr_t)(s->paddr + s->filesz), 0, s->memsz - s->filesz);
	}
	/* translate a virtual entry point to its physical address */
	for (int i = 0; i < nseg; i++) {
		struct elf_seg *s = &segs[i];
		if (entry >= s->vaddr && entry < s->vaddr + s->memsz && s->vaddr != s->paddr) {
			entry = entry - s->vaddr + s->paddr;
			break;
		}
	}
	*entry_out = (u32)entry;
	if (lo_out) *lo_out = lo;
	if (hi_out) *hi_out = hi;
	return 0;
}

/* ---------------- console hand-off ---------------- */
static bool pass_fb;

static void handoff_video(void)
{
	console_flush();
	pass_fb = false;
	if (vga_text_saved() && vga_restore_text()) {
		fb_shutdown();
		return;
	}
	if (fb_is_graphics()) pass_fb = true;
	fb_shutdown();
}

static int build_mmap(u8 *out, int esz, int max)
{
	int n = 0;
	for (int i = 0; i < sysinfo.nmem && n < max; i++) {
		struct memrange *m = &sysinfo.mem[i];
		u8 *e = out + n * esz;
		memset(e, 0, esz);
		*(u64 *)e = m->base;
		*(u64 *)(e + 8) = m->size;
		*(u32 *)(e + 16) = m->type == MEM_CBTABLE ? MEM_RESERVED : m->type;
		n++;
	}
	return n;
}

static void mem_lower_upper(u32 *lower, u32 *upper)
{
	*lower = 0; *upper = 0;
	for (int i = 0; i < sysinfo.nmem; i++) {
		struct memrange *m = &sysinfo.mem[i];
		if (m->type != MEM_RAM) continue;
		if (m->base == 0) *lower = MIN(m->size, 0xa0000ull) / 1024;
		if (m->base <= 0x100000 && m->base + m->size > 0x100000)
			*upper = MIN(m->base + m->size - 0x100000, 0xfff00000ull) / 1024;
	}
	if (!*lower) *lower = 639;
}

static const char loader_name[] = "SunBiosPort (OpenBoot for x86)";

/* ---------------- Multiboot v1 ---------------- */
int multiboot_boot(struct src *k, u8 *hdrbuf, u32 hoff, const char *cmdline,
		   struct src *mods, const char **modcmd, int nmods)
{
	u32 *h = (u32 *)(hdrbuf + hoff);
	u32 flags = h[1];
	u32 entry;
	u64 klo, khi;
	if (flags & 0x10000) {
		u32 header_addr = h[3], load_addr = h[4], load_end = h[5], bss_end = h[6];
		entry = h[7];
		u32 foff = hoff - (header_addr - load_addr);
		u32 lsize = load_end ? load_end - load_addr : (u32)(k->size - foff);
		u32 msize = bss_end ? bss_end - load_addr : lsize;
		if (place_check(load_addr, (u64)load_addr + MAX(lsize, msize)) < 0) return -1;
		if (src_load(k, foff, lsize, load_addr) < 0) return -1;
		if (msize > lsize) memset((void *)(uintptr_t)(load_addr + lsize), 0, msize - lsize);
		klo = load_addr; khi = (u64)load_addr + MAX(lsize, msize);
	} else {
		if (elf_load(k, &entry, &klo, &khi) < 0) {
			printf("Multiboot kernel is neither ELF nor a.out-kludge\n");
			return -1;
		}
	}
	/* modules go after the kernel */
	u32 modinfo[MAX_MODS][4];
	for (int i = 0; i < nmods; i++) {
		u64 a = phys_alloc(ALIGN_UP(mods[i].size, 4096) ? ALIGN_UP(mods[i].size, 4096) : 4096,
				   4096, khi, 0xfffff000ull, false);
		if (!a) { printf("No memory for module %s\n", mods[i].name); return -1; }
		if (src_load(&mods[i], 0, mods[i].size, a) < 0) {
			printf("Can't read module %s\n", mods[i].name);
			return -1;
		}
		modinfo[i][0] = a;
		modinfo[i][1] = a + mods[i].size;
	}
	u64 infobase = phys_alloc(0x10000, 4096, 0x10000, 0x9f000, false);
	if (!infobase) infobase = phys_alloc(0x10000, 4096, 0x100000, 0x40000000, false);
	if (!infobase) return -1;
	u8 *base = (u8 *)(uintptr_t)infobase;
	memset(base, 0, 0x10000);
	u32 *mbi = (u32 *)base;
	char *strings = (char *)base + 0x8000;
	char *sp = strings;
	u32 lower, upper;
	mem_lower_upper(&lower, &upper);
	mbi[0] = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 6) | (1 << 9);
	mbi[1] = lower;
	mbi[2] = upper;
	strcpy(sp, cmdline); mbi[4] = (uintptr_t)sp; sp += strlen(sp) + 1;
	u32 *mods_tbl = (u32 *)(base + 0x200);
	for (int i = 0; i < nmods; i++) {
		mods_tbl[i * 4] = modinfo[i][0];
		mods_tbl[i * 4 + 1] = modinfo[i][1];
		strcpy(sp, modcmd[i]);
		mods_tbl[i * 4 + 2] = (uintptr_t)sp;
		sp += strlen(sp) + 1;
	}
	mbi[5] = nmods;
	mbi[6] = (uintptr_t)mods_tbl;
	/* memory map: entries are {size, base, len, type} with size excluding itself */
	u8 *mm = base + 0x1000;
	u8 tmp[64 * 24];
	int n = build_mmap(tmp, 24, 64);
	for (int i = 0; i < n; i++) {
		u8 *e = mm + i * 24;
		*(u32 *)e = 20;
		memcpy(e + 4, tmp + i * 24, 20);
	}
	mbi[11] = n * 24;
	mbi[12] = (uintptr_t)mm;
	strcpy(sp, loader_name); mbi[16] = (uintptr_t)sp; sp += strlen(sp) + 1;

	printf("\n");
	handoff_video();
	if (pass_fb && (h[1] & 4)) {
		const struct fb_info *f = fb_get_info();
		u8 *b = base;
		mbi[0] |= 1 << 12;
		*(u64 *)(b + 88) = f->base;
		*(u32 *)(b + 96) = f->pitch;
		*(u32 *)(b + 100) = f->width;
		*(u32 *)(b + 104) = f->height;
		b[108] = f->bpp; b[109] = 1;
		b[110] = f->rpos; b[111] = f->rsize; b[112] = f->gpos;
		b[113] = f->gsize; b[114] = f->bpos; b[115] = f->bsize;
	}
	boot_jump(entry, 0x2badb002, (u32)infobase, 0);
}

/* ---------------- Multiboot2 ---------------- */
static int multiboot2_boot(struct src *k, u8 *hdrbuf, u32 hoff, const char *cmdline,
			   struct src *mods, const char **modcmd, int nmods)
{
	u32 *h = (u32 *)(hdrbuf + hoff);
	u32 hlen = h[2];
	u32 entry = 0, addr_tag[4] = { 0 };
	bool have_addr = false, want_fb = false;
	for (u32 o = 16; o + 8 <= hlen;) {
		u8 *t = (u8 *)h + o;
		u16 type = *(u16 *)t, tflags = *(u16 *)(t + 2);
		u32 size = *(u32 *)(t + 4);
		if (type == 0) break;
		switch (type) {
		case 1: case 4: case 6: case 10: break;
		case 2: memcpy(addr_tag, t + 8, 16); have_addr = true; break;
		case 3: entry = *(u32 *)(t + 8); break;
		case 5: want_fb = true; break;
		default:
			if (!(tflags & 1)) {
				printf("Multiboot2 header tag %d not supported\n", type);
				return -1;
			}
		}
		o += ALIGN_UP(size, 8);
	}
	u64 klo, khi;
	if (have_addr) {
		u32 header_addr = addr_tag[0], load_addr = addr_tag[1], load_end = addr_tag[2], bss_end = addr_tag[3];
		u32 foff = hoff - (header_addr - load_addr);
		u32 lsize = load_end ? load_end - load_addr : (u32)(k->size - foff);
		u32 msize = bss_end ? bss_end - load_addr : lsize;
		if (place_check(load_addr, (u64)load_addr + MAX(lsize, msize)) < 0) return -1;
		if (src_load(k, foff, lsize, load_addr) < 0) return -1;
		if (msize > lsize) memset((void *)(uintptr_t)(load_addr + lsize), 0, msize - lsize);
		klo = load_addr; khi = (u64)load_addr + MAX(lsize, msize);
	} else {
		u32 e;
		if (elf_load(k, &e, &klo, &khi) < 0) return -1;
		if (!entry) entry = e;
	}
	if (!entry) return -1;
	u32 modaddr[MAX_MODS];
	for (int i = 0; i < nmods; i++) {
		u64 a = phys_alloc(MAX(ALIGN_UP(mods[i].size, 4096), 4096ull), 4096, khi, 0xfffff000ull, false);
		if (!a || src_load(&mods[i], 0, mods[i].size, a) < 0) {
			printf("Can't load module %s\n", mods[i].name);
			return -1;
		}
		modaddr[i] = a;
	}
	u64 infobase = phys_alloc(0x10000, 4096, 0x10000, 0x9f000, false);
	if (!infobase) infobase = phys_alloc(0x10000, 4096, 0x100000, 0x40000000, false);
	if (!infobase) return -1;
	u8 *b = (u8 *)(uintptr_t)infobase, *p = b + 8;
	memset(b, 0, 0x10000);
#define TAG(type_, len_) do { *(u32 *)p = (type_); *(u32 *)(p + 4) = (len_); } while (0)
#define NEXT() p += ALIGN_UP(*(u32 *)(p + 4), 8)
	TAG(1, 8 + strlen(cmdline) + 1); strcpy((char *)p + 8, cmdline); NEXT();
	TAG(2, 8 + sizeof(loader_name)); strcpy((char *)p + 8, loader_name); NEXT();
	for (int i = 0; i < nmods; i++) {
		TAG(3, 16 + strlen(modcmd[i]) + 1);
		*(u32 *)(p + 8) = modaddr[i];
		*(u32 *)(p + 12) = modaddr[i] + mods[i].size;
		strcpy((char *)p + 16, modcmd[i]);
		NEXT();
	}
	u32 lower, upper;
	mem_lower_upper(&lower, &upper);
	TAG(4, 16); *(u32 *)(p + 8) = lower; *(u32 *)(p + 12) = upper; NEXT();
	int n = build_mmap(p + 16, 24, 64);
	TAG(6, 16 + n * 24); *(u32 *)(p + 8) = 24; *(u32 *)(p + 12) = 0; NEXT();
	uintptr_t rsdp = sysinfo.acpi_rsdp;
	if (!rsdp)
		for (uintptr_t a = 0xe0000; a < 0x100000; a += 16)
			if (!memcmp((void *)a, "RSD PTR ", 8)) { rsdp = a; break; }
	if (rsdp) {
		if (((u8 *)rsdp)[15] >= 2) { TAG(15, 8 + 36); memcpy(p + 8, (void *)rsdp, 36); }
		else { TAG(14, 8 + 20); memcpy(p + 8, (void *)rsdp, 20); }
		NEXT();
	}
	printf("\n");
	handoff_video();
	if (pass_fb && want_fb) {
		const struct fb_info *f = fb_get_info();
		TAG(8, 38);
		*(u64 *)(p + 8) = f->base;
		*(u32 *)(p + 16) = f->pitch;
		*(u32 *)(p + 20) = f->width;
		*(u32 *)(p + 24) = f->height;
		p[28] = f->bpp; p[29] = 1;
		p[32] = f->rpos; p[33] = f->rsize; p[34] = f->gpos;
		p[35] = f->gsize; p[36] = f->bpos; p[37] = f->bsize;
		NEXT();
	} else if (!pass_fb) {
		/* EGA text console */
		TAG(8, 32);
		*(u64 *)(p + 8) = 0xb8000;
		*(u32 *)(p + 16) = 160;
		*(u32 *)(p + 20) = 80;
		*(u32 *)(p + 24) = 25;
		p[28] = 16; p[29] = 2;
		NEXT();
	}
	TAG(0, 8); NEXT();
	*(u32 *)b = p - b;
#undef TAG
#undef NEXT
	boot_jump(entry, 0x36d76289, (u32)infobase, 0);
}

/* ---------------- Linux ---------------- */
int linux_boot(struct src *k, const char *cmdline, struct src *initrd, int ninitrd)
{
	u8 hdr[1024];
	if (src_read(k, 0, sizeof(hdr), hdr) < 0) return -1;
	u16 ver = *(u16 *)(hdr + 0x206);
	if (ver < 0x202 || !(hdr[0x211] & 1)) {
		printf("Linux boot protocol %x.%02x (zImage) not supported\n", ver >> 8, ver & 0xff);
		return -1;
	}
	u32 setup_sects = hdr[0x1f1] ? hdr[0x1f1] : 4;
	u32 koff = (setup_sects + 1) * 512;
	u32 ksize = k->size - koff;
	u32 kaddr = 0x100000;
	if (place_check(kaddr, kaddr + ksize) < 0) return -1;
	u64 zp = phys_alloc(0x3000, 4096, 0x10000, 0x9f000, false);
	if (!zp) return -1;
	u8 *bp = (u8 *)(uintptr_t)zp;
	memset(bp, 0, 0x3000);
	u32 hend = 0x202 + hdr[0x201];
	if (hend > sizeof(hdr)) hend = sizeof(hdr);
	memcpy(bp + 0x1f1, hdr + 0x1f1, hend - 0x1f1);
	/* command line */
	char *cl = (char *)bp + 0x2000;
	u32 maxcl = ver >= 0x206 ? *(u32 *)(hdr + 0x238) : 255;
	if (maxcl > 0xfff) maxcl = 0xfff;
	strlcpy(cl, cmdline, maxcl + 1);
	*(u32 *)(bp + 0x228) = (uintptr_t)cl;
	bp[0x210] = 0xff;		/* type_of_loader: undefined */
	bp[0x211] |= 0x80;		/* CAN_USE_HEAP */
	*(u16 *)(bp + 0x224) = 0xde00;	/* heap_end_ptr */
	*(u32 *)(bp + 0x214) = kaddr;	/* code32_start */
	dprintf("Loading kernel (%u bytes)\n", ksize);
	if (src_load(k, koff, ksize, kaddr) < 0) return -1;
	/* initrds are concatenated, placed as high as the kernel allows */
	if (ninitrd) {
		u64 total = 0;
		for (int i = 0; i < ninitrd; i++) total += ALIGN_UP(initrd[i].size, 4);
		u32 maxaddr = ver >= 0x203 ? *(u32 *)(hdr + 0x22c) : 0x37ffffff;
		u64 a = phys_alloc(ALIGN_UP(total, 4096), 4096, 0x100000, (u64)maxaddr + 1, true);
		if (!a) { printf("No memory for initrd\n"); return -1; }
		u64 o = 0;
		for (int i = 0; i < ninitrd; i++) {
			dprintf("Loading initrd %s (%llu bytes)\n", initrd[i].name, initrd[i].size);
			if (src_load(&initrd[i], 0, initrd[i].size, a + o) < 0) return -1;
			o += ALIGN_UP(initrd[i].size, 4);
		}
		*(u32 *)(bp + 0x218) = a;
		*(u32 *)(bp + 0x21c) = total;
	}
	/* e820 */
	int n = 0;
	for (int i = 0; i < sysinfo.nmem && n < 128; i++, n++) {
		u8 *e = bp + 0x2d0 + n * 20;
		*(u64 *)e = sysinfo.mem[i].base;
		*(u64 *)(e + 8) = sysinfo.mem[i].size;
		*(u32 *)(e + 16) = sysinfo.mem[i].type == MEM_CBTABLE ? MEM_RESERVED : sysinfo.mem[i].type;
	}
	bp[0x1e8] = n;
	if (sysinfo.acpi_rsdp) *(u64 *)(bp + 0x070) = sysinfo.acpi_rsdp;
	console_flush();
	handoff_video();
	if (pass_fb) {
		const struct fb_info *f = fb_get_info();
		bp[0x0f] = 0x23;			/* VIDEO_TYPE_VLFB */
		*(u16 *)(bp + 0x12) = f->width;
		*(u16 *)(bp + 0x14) = f->height;
		*(u16 *)(bp + 0x16) = f->bpp;
		*(u32 *)(bp + 0x18) = f->base;
		*(u32 *)(bp + 0x1c) = ALIGN_UP(f->pitch * f->height, 65536) >> 16;
		*(u16 *)(bp + 0x24) = f->pitch;
		bp[0x26] = f->rsize; bp[0x27] = f->rpos;
		bp[0x28] = f->gsize; bp[0x29] = f->gpos;
		bp[0x2a] = f->bsize; bp[0x2b] = f->bpos;
		bp[0x2c] = 0; bp[0x2d] = 0;
		*(u16 *)(bp + 0x34) = 0xffff;		/* capabilities: 64-bit base valid */
		*(u32 *)(bp + 0x3a) = 0;		/* ext_lfb_base */
	} else {
		bp[0x0f] = 0x22;			/* VIDEO_TYPE_VGAC */
		bp[0x06] = 0;				/* orig_video_page */
		bp[0x07] = 80;				/* orig_video_cols */
		bp[0x0e] = 25;				/* orig_video_lines */
		*(u16 *)(bp + 0x10) = 16;		/* orig_video_points */
		bp[0x00] = 0; bp[0x01] = 0;
	}
	boot_jump(kaddr, 0, 0, (u32)zp);
}

/* ---------------- plain ELF / coreboot payload ---------------- */
int elf_boot(struct src *k, const char *cmdline)
{
	(void)cmdline;
	u32 entry;
	if (elf_load(k, &entry, NULL, NULL) < 0) return -1;
	console_flush();
	fb_shutdown();
	call_payload(entry, sysinfo.cb_header);
}

static int find_mb_header(const u8 *buf, u32 len, u32 *off, int *ver)
{
	for (u32 o = 0; o + 12 <= len && o < 8192; o += 4) {
		const u32 *w = (const u32 *)(buf + o);
		if (w[0] == 0x1badb002 && w[0] + w[1] + w[2] == 0) { *off = o; *ver = 1; return 0; }
	}
	for (u32 o = 0; o + 16 <= len && o < 32768; o += 8) {
		const u32 *w = (const u32 *)(buf + o);
		if (w[0] == 0xe85250d6 && w[1] == 0 && w[0] + w[1] + w[2] + w[3] == 0) {
			*off = o; *ver = 2; return 0;
		}
	}
	return -1;
}

int boot_load_image(struct src *k, const char *cmdline, struct src *mods, const char **modcmd,
		    int nmods, struct src *initrd, int ninitrd)
{
	u32 hl = MIN(k->size, 32768ull);
	u8 *hb = malloc(32768);
	if (!hb) return -1;
	memset(hb, 0, 32768);
	if (src_read(k, 0, hl, hb) < 0) { free(hb); printf("Can't read %s\n", k->name); return -1; }
	u32 off;
	int ver, r;
	if (hl > 0x210 && *(u16 *)(hb + 0x1fe) == 0xaa55 && !memcmp(hb + 0x202, "HdrS", 4)) {
		/* a Linux kernel may also carry multiboot modules as initrds */
		if (nmods && !ninitrd) { initrd = mods; ninitrd = nmods; }
		r = linux_boot(k, cmdline, initrd, ninitrd);
	} else if (find_mb_header(hb, hl, &off, &ver) == 0) {
		r = ver == 1 ? multiboot_boot(k, hb, off, cmdline, mods, modcmd, nmods)
			     : multiboot2_boot(k, hb, off, cmdline, mods, modcmd, nmods);
	} else if (!memcmp(hb, "\177ELF", 4)) {
		r = elf_boot(k, cmdline);
	} else {
		printf("%s: unrecognized executable format\n", k->name);
		r = -1;
	}
	free(hb);
	return r;
}
