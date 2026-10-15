/* coreboot CBFS access: list files and start SELF payloads such as SeaBIOS
 * ("img/seabios"), which provides the legacy PC BIOS needed by older x86
 * Solaris releases (2.5.1 - 9) and anything else that wants INT 13h. */
#include "boot.h"
#include "platform.h"
#include "console.h"

static u32 be32(const void *p) { const u8 *b = p; return ((u32)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; }
static u64 be64(const void *p) { return ((u64)be32(p) << 32) | be32((const u8 *)p + 4); }

static bool cbfs_region(uintptr_t *base, u32 *size)
{
	if (sysinfo.have_bootmedia && sysinfo.boot_media_size && sysinfo.boot_media_size <= 0x10000000ull) {
		u64 flash = 0x100000000ull - sysinfo.boot_media_size;
		*base = (uintptr_t)(flash + sysinfo.cbfs_offset);
		*size = sysinfo.cbfs_size;
		return true;
	}
	if (!sysinfo.from_coreboot) return false;
	u32 ptr = *(volatile u32 *)0xfffffffc;
	uintptr_t hdr = ptr >= 0xff000000u ? ptr : (uintptr_t)(0x100000000ull + (s32)ptr);
	if (hdr < 0xff000000u || be32((void *)hdr) != 0x4f524243) return false;	/* "ORBC" */
	u32 romsize = be32((u8 *)hdr + 8), offset = be32((u8 *)hdr + 20);
	*base = (uintptr_t)(0x100000000ull - romsize + offset);
	*size = romsize - offset;
	return true;
}

typedef int (*cbfs_cb)(void *arg, const char *name, u32 type, const u8 *data, u32 len);

static int cbfs_walk(cbfs_cb cb, void *arg)
{
	uintptr_t base;
	u32 size;
	if (!cbfs_region(&base, &size)) return -1;
	for (u32 off = 0; off + 24 < size;) {
		const u8 *h = (const u8 *)(base + off);
		if (memcmp(h, "LARCHIVE", 8)) { off += 64; continue; }
		u32 len = be32(h + 8), type = be32(h + 12), doff = be32(h + 20);
		if (doff < 24 || doff > 4096) { off += 64; continue; }
		int r = cb(arg, (const char *)h + 24, type, h + doff, len);
		if (r) return r;
		off = ALIGN_UP(off + doff + len, 64);
	}
	return 0;
}

static int list_cb(void *arg, const char *name, u32 type, const u8 *data, u32 len)
{
	(void)arg; (void)data;
	const char *t = type == 0x20 ? "payload" : type == 0x10 ? "stage" : type == 0x50 ? "raw" :
			type == 0x30 ? "optionrom" : type == 0x02 ? "cbfs header" : type == 0xffffffff ? "(empty)" : "";
	printf("%-36s %8u  %s\n", name[0] ? name : "(empty)", len, t);
	return 0;
}

void cbfs_list(void)
{
	if (cbfs_walk(list_cb, NULL) < 0) printf("No CBFS (not running from coreboot?)\n");
}

struct find { const char *name; const u8 *data; u32 len, type; };

static int find_cb(void *arg, const char *name, u32 type, const u8 *data, u32 len)
{
	struct find *f = arg;
	if (strcmp(name, f->name)) return 0;
	f->data = data; f->len = len; f->type = type;
	return 1;
}

int cbfs_boot(const char *name)
{
	struct find f = { name, NULL, 0, 0 };
	if (cbfs_walk(find_cb, &f) < 0) { printf("No CBFS (not running from coreboot?)\n"); return -1; }
	if (!f.data) { printf("CBFS file %s not found\n", name); return -1; }
	if (f.type != 0x20) { printf("%s is not a payload\n", name); return -1; }
	phys_reset();
	u32 entry = 0;
	/* first pass: check placement */
	for (const u8 *s = f.data; s < f.data + f.len; s += 28) {
		u32 type = be32(s);
		if (type == 0x454e5452) break;	/* ENTR */
		if (type == 0x50415241) continue;	/* PARA */
		u64 load = be64(s + 12);
		u32 mlen = be32(s + 24);
		if (!phys_free_range(load, load + mlen) && load >= 0x100000) {
			printf("Payload segment %llx-%llx overlaps the firmware\n", load, load + mlen);
			return -1;
		}
	}
	for (const u8 *s = f.data; s < f.data + f.len; s += 28) {
		u32 type = be32(s), comp = be32(s + 4), off = be32(s + 8);
		u64 load = be64(s + 12);
		u32 len = be32(s + 20), mlen = be32(s + 24);
		if (type == 0x454e5452) { entry = (u32)load; break; }
		if (type == 0x50415241) continue;
		u8 *dst = (u8 *)(uintptr_t)load;
		if (type == 0x42535320) { memset(dst, 0, mlen); continue; }	/* "BSS " */
		/* copy compressed data out of flash first: it is slow and may alias */
		u8 *tmp = malloc(len);
		if (!tmp) return -1;
		memcpy(tmp, f.data + off, len);
		u32 got = len;
		int r = 0;
		if (comp == 0) memcpy(dst, tmp, len);
		else if (comp == 1) r = lzma_decompress(tmp, len, dst, mlen, &got);
		else if (comp == 2) r = lz4_decompress(tmp, len, dst, mlen, &got);
		else r = -1;
		free(tmp);
		if (r < 0) { printf("Can't decompress %s segment at %llx\n", name, load); return -1; }
		if (got < mlen) memset(dst + got, 0, mlen - got);
	}
	if (!entry) return -1;
	console_flush();
	if (vga_text_saved()) vga_restore_text();
	fb_shutdown();
	call_payload(entry, sysinfo.cb_header);
}
