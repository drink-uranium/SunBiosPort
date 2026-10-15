/* Partition tables (MBR, extended, GPT, Solaris x86 VTOC) and the generic
 * path walker shared by the filesystem drivers. */
#include "fs.h"

static const struct fs_ops *fs_drivers[] = { &iso9660_ops, &ufs_ops, &ext2_ops, &fat_ops };

static void add_vol(struct volume *vols, int *n, int max, struct blockdev *bd, u64 start_lba,
		    u64 nsec, const char *desc)
{
	if (*n >= max || !nsec) return;
	struct volume *v = &vols[(*n)++];
	v->bd = bd;
	v->start = start_lba * 512;
	v->size = nsec * 512;
	strlcpy(v->desc, desc, sizeof(v->desc));
}

/* Solaris x86 VTOC lives in sector 1 of an fdisk partition of type 0xbf/0x82. */
static void scan_vtoc(struct blockdev *bd, u64 base, struct volume *vols, int *n, int max, int pno)
{
	struct volume whole = { bd, 0, bd->nsectors * bd->sector_size, "" };
	u8 lbl[512];
	if (vol_read(&whole, (base + 1) * 512, 512, lbl) < 0) return;
	if (*(u32 *)(lbl + 12) != 0x600ddeee) return;
	u16 np = *(u16 *)(lbl + 30);
	if (np > 16) np = 16;
	/* root slices first, then everything except swap and backup */
	for (int pass = 0; pass < 2; pass++) {
		for (int s = 0; s < np; s++) {
			u8 *e = lbl + 72 + s * 12;
			u16 tag = *(u16 *)e;
			u32 st = *(u32 *)(e + 4), sz = *(u32 *)(e + 8);
			if (!sz || tag == 3 || tag == 5) continue;
			if ((pass == 0) != (tag == 2)) continue;
			char d[48];
			snprintf(d, sizeof(d), "fdisk %d, slice s%d", pno, s);
			add_vol(vols, n, max, bd, base + st, sz, d);
		}
	}
}

static void scan_gpt(struct blockdev *bd, struct volume *vols, int *n, int max)
{
	struct volume whole = { bd, 0, bd->nsectors * bd->sector_size, "" };
	u32 ss = bd->sector_size;
	u8 hdr[512];
	if (vol_read(&whole, ss, 512, hdr) < 0 || memcmp(hdr, "EFI PART", 8)) return;
	u64 lba = *(u64 *)(hdr + 72);
	u32 cnt = *(u32 *)(hdr + 80), esz = *(u32 *)(hdr + 84);
	if (esz < 128 || cnt > 256) return;
	for (u32 i = 0; i < cnt; i++) {
		u8 e[128];
		if (vol_read(&whole, lba * ss + (u64)i * esz, 128, e) < 0) return;
		static const u8 zero[16];
		if (!memcmp(e, zero, 16)) continue;
		u64 first = *(u64 *)(e + 32), last = *(u64 *)(e + 40);
		if (*n >= max || last < first) continue;
		struct volume *v = &vols[(*n)++];
		v->bd = bd;
		v->start = first * ss;
		v->size = (last - first + 1) * ss;
		snprintf(v->desc, sizeof(v->desc), "gpt %u", i + 1);
	}
}

int part_scan(struct blockdev *bd, struct volume *vols, int max)
{
	int n = 0;
	if (max <= 0) return 0;
	vols[0].bd = bd;
	vols[0].start = 0;
	vols[0].size = bd->nsectors * bd->sector_size;
	strlcpy(vols[0].desc, "whole device", sizeof(vols[0].desc));
	n = 1;
	if (bd->type == BDEV_CDROM) return n;
	u8 mbr[512];
	if (vol_read(&vols[0], 0, 512, mbr) < 0) return n;
	if (mbr[510] != 0x55 || mbr[511] != 0xaa) return n;
	for (int i = 0; i < 4; i++) {
		u8 *e = mbr + 446 + i * 16;
		u8 type = e[4];
		u32 st = *(u32 *)(e + 8), sz = *(u32 *)(e + 12);
		if (!type || !sz) continue;
		if (type == 0xee) { scan_gpt(bd, vols, &n, max); continue; }
		if (type == 0xbf || type == 0x82) scan_vtoc(bd, st, vols, &n, max, i + 1);
		if (type == 0x05 || type == 0x0f || type == 0x85) {
			u32 ebr = st;
			for (int k = 0; k < 32; k++) {
				u8 b[512];
				if (vol_read(&vols[0], (u64)ebr * 512, 512, b) < 0 || b[510] != 0x55) break;
				u8 *l = b + 446;
				if (l[4] && *(u32 *)(l + 12)) {
					char d[48];
					snprintf(d, sizeof(d), "logical %d", k + 5);
					add_vol(vols, &n, max, bd, ebr + *(u32 *)(l + 8), *(u32 *)(l + 12), d);
				}
				u8 *nx = b + 446 + 16;
				if (!nx[4]) break;
				ebr = st + *(u32 *)(nx + 8);
			}
			continue;
		}
		char d[48];
		snprintf(d, sizeof(d), "fdisk %d (type %02x)", i + 1, type);
		add_vol(vols, &n, max, bd, st, sz, d);
	}
	return n;
}

int fs_mount(struct volume *v, struct fs *fs)
{
	for (u32 i = 0; i < ARRAY_SIZE(fs_drivers); i++) {
		memset(fs, 0, sizeof(*fs));
		fs->vol = *v;
		fs->ops = fs_drivers[i];
		if (fs->ops->mount(fs) == 0) return 0;
	}
	fs->ops = NULL;
	return -1;
}

struct find_ctx { const char *name; size_t len; bool ci; bool found; struct fnode node; };

static void find_cb(void *arg, const char *name, const struct fnode *n)
{
	struct find_ctx *c = arg;
	if (c->found || strlen(name) != c->len) return;
	if (c->ci ? strncasecmp(name, c->name, c->len) : strncmp(name, c->name, c->len)) return;
	c->found = true;
	c->node = *n;
}

static int walk(struct fs *fs, const char *path, struct fnode *out, int depth)
{
	struct fnode cur, stack[32];
	int sp = 0;
	if (depth > 8) return -1;
	if (fs->ops->root(fs, &cur) < 0) return -1;
	stack[sp++] = cur;
	const char *p = path;
	while (*p) {
		while (*p == '/') p++;
		if (!*p) break;
		const char *e = p;
		while (*e && *e != '/') e++;
		size_t len = e - p;
		if (len == 1 && p[0] == '.') { p = e; continue; }
		if (len == 2 && p[0] == '.' && p[1] == '.') {
			if (sp > 1) sp--;
			cur = stack[sp - 1];
			p = e;
			continue;
		}
		if (!cur.dir) return -1;
		struct find_ctx c = { p, len, fs->ci || fs->ops->case_insensitive, false, { 0 } };
		fs->ops->readdir(fs, &cur, find_cb, &c);
		if (!c.found) return -1;
		if (c.node.link) {
			char target[256], np[512];
			u32 l = MIN(c.node.size, sizeof(target) - 1);
			if (fs->ops->read(fs, &c.node, 0, l, target) < 0) return -1;
			target[l] = 0;
			if (target[0] == '/') {
				snprintf(np, sizeof(np), "%s%s", target, e);
			} else {
				/* rebuild the parent directory path */
				char parent[256];
				size_t plen = p - path;
				if (plen >= sizeof(parent)) return -1;
				memcpy(parent, path, plen);
				parent[plen] = 0;
				snprintf(np, sizeof(np), "%s%s%s", parent, target, e);
			}
			return walk(fs, np, out, depth + 1);
		}
		cur = c.node;
		if (sp < 32) stack[sp++] = cur;
		p = e;
	}
	*out = cur;
	return 0;
}

int fs_open(struct fs *fs, const char *path, struct file *f)
{
	if (!fs->ops) return -1;
	if (walk(fs, path, &f->node, 0) < 0) return -1;
	f->fs = fs;
	f->size = f->node.size;
	return 0;
}

int fs_read(struct file *f, u64 off, u32 len, void *buf)
{
	if (off >= f->size) return 0;
	if (off + len > f->size) len = f->size - off;
	if (f->fs->ops->read(f->fs, &f->node, off, len, buf) < 0) return -1;
	return len;
}

int fs_list(struct fs *fs, const char *path, dir_cb cb, void *arg)
{
	struct fnode n;
	if (walk(fs, path, &n, 0) < 0 || !n.dir) return -1;
	return fs->ops->readdir(fs, &n, cb, arg);
}
