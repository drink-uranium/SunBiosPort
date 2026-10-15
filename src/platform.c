/* Discovery of the environment we were started in: coreboot tables,
 * Multiboot information, the memory map, SMBIOS, and system reset. */
#include "platform.h"
#include "pci.h"

struct sysinfo sysinfo;

/* ---------------- coreboot tables ---------------- */
struct cb_header { char sig[4]; u32 header_bytes, header_csum, table_bytes, table_csum, entries; };
struct cb_record { u32 tag, size; };

static u16 ipchksum(const void *p, size_t n)
{
	const u8 *b = p;
	u32 sum = 0;
	for (size_t i = 0; i < n; i++) sum += (i & 1) ? (u32)b[i] << 8 : b[i];
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return ~sum & 0xffff;
}

static struct cb_header *cb_find_at(uintptr_t start, uintptr_t end)
{
	for (uintptr_t a = start; a < end; a += 16) {
		struct cb_header *h = (struct cb_header *)a;
		if (memcmp(h->sig, "LBIO", 4)) continue;
		if (h->header_bytes != sizeof(*h) || ipchksum(h, sizeof(*h))) continue;
		if (ipchksum((u8 *)h + h->header_bytes, h->table_bytes) != h->table_csum) continue;
		return h;
	}
	return NULL;
}

static u64 cb64(const void *p) { const u32 *w = p; return w[0] | ((u64)w[1] << 32); }

static void cb_parse(struct cb_header *h, int depth)
{
	u8 *p = (u8 *)h + h->header_bytes;
	sysinfo.cb_header = (uintptr_t)h;
	for (u32 i = 0; i < h->entries; i++) {
		struct cb_record *r = (struct cb_record *)p;
		u8 *d = p + 8;
		switch (r->tag) {
		case 0x0001: { /* memory */
			int n = (r->size - 8) / 20;
			sysinfo.nmem = 0;
			for (int j = 0; j < n && sysinfo.nmem < 64; j++) {
				struct memrange *m = &sysinfo.mem[sysinfo.nmem++];
				m->base = cb64(d + j * 20);
				m->size = cb64(d + j * 20 + 8);
				m->type = *(u32 *)(d + j * 20 + 16);
			}
			break;
		}
		case 0x0003: { /* mainboard */
			u8 vi = d[0], pi = d[1];
			strlcpy(sysinfo.cb_vendor, (char *)d + 2 + vi, sizeof(sysinfo.cb_vendor));
			strlcpy(sysinfo.cb_part, (char *)d + 2 + pi, sizeof(sysinfo.cb_part));
			break;
		}
		case 0x0004: /* version */
			strlcpy(sysinfo.cb_version, (char *)d, sizeof(sysinfo.cb_version));
			break;
		case 0x000f: { /* serial */
			u32 *s = (u32 *)d;
			sysinfo.have_serial = true;
			if (s[0] == 1) sysinfo.serial_port = s[1];
			else sysinfo.serial_mmio = s[1];
			sysinfo.serial_baud = s[2];
			sysinfo.serial_regw = s[3];
			break;
		}
		case 0x0011: /* forward */
			if (depth < 3) {
				struct cb_header *f = (struct cb_header *)(uintptr_t)cb64(d);
				if (!memcmp(f->sig, "LBIO", 4)) { cb_parse(f, depth + 1); return; }
			}
			break;
		case 0x0012: { /* framebuffer */
			struct fb_info *f = &sysinfo.fb;
			f->base = (uintptr_t)cb64(d);
			f->width = *(u32 *)(d + 8);
			f->height = *(u32 *)(d + 12);
			f->pitch = *(u32 *)(d + 16);
			f->bpp = d[20];
			f->rpos = d[21]; f->rsize = d[22];
			f->gpos = d[23]; f->gsize = d[24];
			f->bpos = d[25]; f->bsize = d[26];
			sysinfo.have_fb = f->base && f->width;
			break;
		}
		case 0x0030: /* boot media params */
			sysinfo.cbfs_offset = cb64(d + 8);
			sysinfo.cbfs_size = cb64(d + 16);
			sysinfo.boot_media_size = cb64(d + 24);
			sysinfo.have_bootmedia = true;
			break;
		case 0x0043: /* ACPI RSDP */
			sysinfo.acpi_rsdp = (uintptr_t)cb64(d);
			break;
		}
		p += r->size;
	}
}

/* ---------------- multiboot ---------------- */
static void mb_parse(u32 *mbi)
{
	u32 flags = mbi[0];
	if (flags & (1 << 6)) {
		u8 *p = (u8 *)mbi[12], *end = p + mbi[11];
		sysinfo.nmem = 0;
		while (p < end && sysinfo.nmem < 64) {
			u32 sz = *(u32 *)p;
			struct memrange *m = &sysinfo.mem[sysinfo.nmem++];
			m->base = *(u64 *)(p + 4);
			m->size = *(u64 *)(p + 12);
			m->type = *(u32 *)(p + 20);
			p += sz + 4;
		}
	} else if (flags & 1) {
		sysinfo.mem[0] = (struct memrange){ 0, (u64)mbi[1] * 1024, MEM_RAM };
		sysinfo.mem[1] = (struct memrange){ 0x100000, (u64)mbi[2] * 1024, MEM_RAM };
		sysinfo.nmem = 2;
	}
	if (flags & (1 << 2)) strlcpy(sysinfo.mb_cmdline, (char *)mbi[4], sizeof(sysinfo.mb_cmdline));
	if (flags & (1 << 9)) strlcpy(sysinfo.mb_loader, (char *)mbi[16], sizeof(sysinfo.mb_loader));
	if ((flags & (1 << 12)) && ((u8 *)mbi)[109] == 1) {
		u8 *b = (u8 *)mbi;
		struct fb_info *f = &sysinfo.fb;
		f->base = (uintptr_t)*(u64 *)(b + 88);
		f->pitch = *(u32 *)(b + 96);
		f->width = *(u32 *)(b + 100);
		f->height = *(u32 *)(b + 104);
		f->bpp = b[108];
		f->rpos = b[110]; f->rsize = b[111];
		f->gpos = b[112]; f->gsize = b[113];
		f->bpos = b[114]; f->bsize = b[115];
		sysinfo.have_fb = true;
	}
}

static bool detect_bios(void)
{
	/* A resident PC BIOS has INT 10h/13h/19h vectors pointing into ROM
	 * segments. coreboot's own option ROM IDT points into low memory. */
	u32 v10 = read32(0x10 * 4), v13 = read32(0x13 * 4), v19 = read32(0x19 * 4);
	u16 s10 = v10 >> 16, s13 = v13 >> 16, s19 = v19 >> 16;
	return s13 >= 0xc000 && s19 >= 0xc000 && s10 >= 0xc000 && v13 != 0xffffffff;
}

static void cmos_memory(void)
{
	outb(0x70, 0x30); u32 ext = inb(0x71);
	outb(0x70, 0x31); ext |= inb(0x71) << 8;
	outb(0x70, 0x34); u32 hi = inb(0x71);
	outb(0x70, 0x35); hi |= inb(0x71) << 8;
	sysinfo.mem[0] = (struct memrange){ 0, 640 * 1024, MEM_RAM };
	sysinfo.mem[1] = (struct memrange){ 0x100000, (u64)ext * 1024, MEM_RAM };
	sysinfo.nmem = 2;
	if (hi) {
		sysinfo.mem[1].size = 15 * 1024 * 1024;
		sysinfo.mem[2] = (struct memrange){ 0x1000000, (u64)hi * 65536, MEM_RAM };
		sysinfo.nmem = 3;
	}
}

void platform_init(void)
{
	struct cb_header *h = NULL;
	if (boot_arg && boot_arg < 0xfffff000 && !memcmp((void *)boot_arg, "LBIO", 4))
		h = (struct cb_header *)boot_arg;
	if (!h) h = cb_find_at(0x500, 0x1000);
	if (!h) h = cb_find_at(0xf0000, 0x100000);
	if (h) {
		sysinfo.from_coreboot = true;
		cb_parse(h, 0);
	}
	if (boot_magic == 0x2badb002 && boot_mbi) {
		sysinfo.from_multiboot = true;
		struct memrange save[64];
		int nsave = sysinfo.nmem;
		memcpy(save, sysinfo.mem, sizeof(save));
		mb_parse((u32 *)boot_mbi);
		/* coreboot's own map is more precise when we have both */
		if (nsave) { memcpy(sysinfo.mem, save, sizeof(save)); sysinfo.nmem = nsave; }
	}
	if (!sysinfo.nmem) cmos_memory();
	sysinfo.bios_present = detect_bios();
	sysinfo.total_ram = 0;
	for (int i = 0; i < sysinfo.nmem; i++)
		if (sysinfo.mem[i].type == MEM_RAM) sysinfo.total_ram += sysinfo.mem[i].size;
}

bool mem_is_ram(u64 base, u64 size)
{
	for (int i = 0; i < sysinfo.nmem; i++) {
		struct memrange *m = &sysinfo.mem[i];
		if (m->type == MEM_RAM && base >= m->base && base + size <= m->base + m->size)
			return true;
	}
	return false;
}

u64 mem_top_below(u64 limit)
{
	u64 top = 0;
	for (int i = 0; i < sysinfo.nmem; i++) {
		struct memrange *m = &sysinfo.mem[i];
		if (m->type != MEM_RAM) continue;
		u64 e = m->base + m->size;
		if (e > limit) e = limit;
		if (e > top && e > m->base) top = e;
	}
	return top;
}

/* ---------------- SMBIOS ---------------- */
static const char *smbios_str(u8 *s, int idx)
{
	u8 *p = s + s[1];
	if (!idx) return "";
	while (--idx && *p) p += strlen((char *)p) + 1;
	return (char *)p;
}

void smbios_init(void)
{
	uintptr_t table = 0;
	u32 len = 0;
	for (uintptr_t a = 0xf0000; a < 0x100000; a += 16) {
		if (!memcmp((void *)a, "_SM3_", 5)) {
			table = (uintptr_t)*(u64 *)(a + 0x10);
			len = *(u32 *)(a + 0x0c);
			break;
		}
		if (!memcmp((void *)a, "_SM_", 4)) {
			table = *(u32 *)(a + 0x18);
			len = *(u16 *)(a + 0x16);
			break;
		}
	}
	if (!table) return;
	u8 *p = (u8 *)table, *end = p + len;
	while (p + 4 <= end) {
		u8 type = p[0], hl = p[1];
		if (type == 127 || hl < 4) break;
		if (type == 0) {
			strlcpy(sysinfo.bios_version, smbios_str(p, p[5]), 64);
		} else if (type == 1) {
			strlcpy(sysinfo.sys_vendor, smbios_str(p, p[4]), 64);
			strlcpy(sysinfo.sys_product, smbios_str(p, p[5]), 64);
			strlcpy(sysinfo.sys_serial, smbios_str(p, p[7]), 64);
			if (hl >= 0x19) memcpy(sysinfo.sys_uuid, p + 8, 16);
		} else if (type == 17 && sysinfo.ndimms < 16) {
			u16 sz = *(u16 *)(p + 0x0c);
			u32 mb = 0;
			if (sz == 0x7fff && hl >= 0x20) mb = *(u32 *)(p + 0x1c);
			else if (sz != 0xffff) mb = (sz & 0x8000) ? (sz & 0x7fff) / 1024 : sz;
			int i = sysinfo.ndimms++;
			sysinfo.dimm_mb[i] = mb;
			strlcpy(sysinfo.dimm_loc[i], smbios_str(p, p[0x10]), 24);
		}
		u8 *q = p + hl;
		while (q + 1 < end && (q[0] || q[1])) q++;
		p = q + 2;
	}
}

/* ---------------- reset / power ---------------- */
void reset_system(void)
{
	console_flush();
	outb(0xcf9, 0x02);
	udelay(50);
	outb(0xcf9, 0x06);
	mdelay(50);
	for (int i = 0; i < 1000; i++) {	/* keyboard controller pulse */
		if (!(inb(0x64) & 2)) break;
		udelay(10);
	}
	outb(0x64, 0xfe);
	mdelay(50);
	/* triple fault */
	struct { u16 l; u32 b; } PACKED idt = { 0, 0 };
	__asm__ volatile("lidt %0; int $3" ::"m"(idt));
	for (;;) __asm__ volatile("hlt");
}

static bool acpi_sum(const void *p, u32 n)
{
	u8 s = 0;
	for (u32 i = 0; i < n; i++) s += ((u8 *)p)[i];
	return s == 0;
}

static uintptr_t find_rsdp(void)
{
	if (sysinfo.acpi_rsdp) return sysinfo.acpi_rsdp;
	uintptr_t ebda = (uintptr_t)read16(0x40e) << 4;
	uintptr_t ranges[2][2] = { { ebda, ebda + 1024 }, { 0xe0000, 0x100000 } };
	for (int r = 0; r < 2; r++)
		for (uintptr_t a = ranges[r][0]; a && a < ranges[r][1]; a += 16)
			if (!memcmp((void *)a, "RSD PTR ", 8) && acpi_sum((void *)a, 20)) return a;
	return 0;
}

/* Find \_S5 in the DSDT for SLP_TYPa; fall back to common chipset values. */
static int find_s5(u8 *dsdt, u32 len)
{
	for (u32 i = 0; i + 8 < len; i++) {
		if (memcmp(dsdt + i, "_S5_", 4)) continue;
		u8 *p = dsdt + i + 4;
		if (*p != 0x12) continue;	/* PackageOp */
		p++;
		p += ((*p >> 6) & 3) + 1;	/* PkgLength */
		p++;				/* NumElements */
		if (*p == 0x0a) p++;		/* BytePrefix */
		return *p;
	}
	return -1;
}

void power_off(void)
{
	console_flush();
	uintptr_t rsdp = find_rsdp();
	if (rsdp) {
		u32 rsdt = *(u32 *)(rsdp + 16);
		u32 n = (*(u32 *)(rsdt + 4) - 36) / 4;
		for (u32 i = 0; i < n; i++) {
			u32 t = ((u32 *)(rsdt + 36))[i];
			if (memcmp((void *)t, "FACP", 4)) continue;
			u32 pm1a = *(u32 *)(t + 64), pm1b = *(u32 *)(t + 68);
			u32 dsdt = *(u32 *)(t + 40);
			int s5 = find_s5((u8 *)dsdt, *(u32 *)(dsdt + 4));
			if (s5 < 0) s5 = 5;
			/* make sure ACPI mode is enabled */
			u32 smi = *(u32 *)(t + 48);
			u8 en = *(u8 *)(t + 52);
			if (smi && en && !(inw(pm1a) & 1)) {
				outb(smi, en);
				for (int k = 0; k < 300 && !(inw(pm1a) & 1); k++) mdelay(10);
			}
			outw(pm1a, (s5 << 10) | (1 << 13));
			if (pm1b) outw(pm1b, (s5 << 10) | (1 << 13));
			mdelay(100);
		}
	}
	outw(0x604, 0x2000);	/* QEMU q35/i440fx fallbacks */
	outw(0xb004, 0x2000);
	printf("Power off failed; it is now safe to turn off the system.\n");
	for (;;) __asm__ volatile("cli; hlt");
}
