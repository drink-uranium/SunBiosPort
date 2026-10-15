/* FAT12/16/32 with VFAT long file names. */
#include "fs.h"

struct fat {
	u32 bps, spc, clus_bytes;
	u32 fat_start;		/* bytes */
	u32 root_start, root_bytes;	/* FAT12/16 fixed root dir */
	u64 data_start;
	u32 nclusters, root_clus;
	int type;		/* 12, 16, 32 */
};

static int fat_mount(struct fs *fs)
{
	u8 b[512];
	if (vol_read(&fs->vol, 0, 512, b) < 0) return -1;
	if (b[510] != 0x55 || b[511] != 0xaa) return -1;
	u32 bps = *(u16 *)(b + 11), spc = b[13], rsvd = *(u16 *)(b + 14), nfats = b[16];
	u32 rootent = *(u16 *)(b + 17), tot16 = *(u16 *)(b + 19), fatsz16 = *(u16 *)(b + 22);
	u32 tot32 = *(u32 *)(b + 32), fatsz32 = *(u32 *)(b + 36);
	if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc || (spc & (spc - 1)) || !nfats || !rsvd)
		return -1;
	if (b[0] != 0xeb && b[0] != 0xe9) return -1;
	struct fat *f = zalloc(sizeof(*f));
	u32 fatsz = fatsz16 ? fatsz16 : fatsz32;
	u32 tot = tot16 ? tot16 : tot32;
	u32 rootsecs = (rootent * 32 + bps - 1) / bps;
	f->bps = bps; f->spc = spc; f->clus_bytes = bps * spc;
	f->fat_start = rsvd * bps;
	f->root_start = (rsvd + nfats * fatsz) * bps;
	f->root_bytes = rootsecs * bps;
	f->data_start = (u64)(rsvd + nfats * fatsz + rootsecs) * bps;
	f->nclusters = (tot - (rsvd + nfats * fatsz + rootsecs)) / spc;
	f->type = f->nclusters < 4085 ? 12 : f->nclusters < 65525 ? 16 : 32;
	if (f->type == 32) f->root_clus = *(u32 *)(b + 44);
	fs->priv = f;
	return 0;
}

static u32 next_clus(struct fs *fs, u32 c)
{
	struct fat *f = fs->priv;
	u8 v[4] = { 0 };
	switch (f->type) {
	case 12: {
		vol_read(&fs->vol, f->fat_start + c + c / 2, 2, v);
		u16 x = v[0] | (v[1] << 8);
		x = (c & 1) ? x >> 4 : x & 0xfff;
		return x >= 0xff8 ? 0xffffffff : x;
	}
	case 16:
		vol_read(&fs->vol, f->fat_start + c * 2, 2, v);
		return (v[0] | (v[1] << 8)) >= 0xfff8 ? 0xffffffff : (u32)(v[0] | (v[1] << 8));
	default:
		vol_read(&fs->vol, f->fat_start + c * 4, 4, v);
		u32 x = *(u32 *)v & 0x0fffffff;
		return x >= 0x0ffffff8 ? 0xffffffff : x;
	}
}

static int fat_root(struct fs *fs, struct fnode *n)
{
	struct fat *f = fs->priv;
	memset(n, 0, sizeof(*n));
	n->dir = true;
	n->a = f->type == 32 ? f->root_clus : 0;
	n->size = 0xffffffff;
	return 0;
}

static int fat_read(struct fs *fs, struct fnode *n, u64 off, u32 len, void *buf)
{
	struct fat *f = fs->priv;
	u8 *out = buf;
	if (n->a == 0 && n->dir) {	/* fixed root */
		if (off + len > f->root_bytes) return -1;
		return vol_read(&fs->vol, f->root_start + off, len, buf);
	}
	u32 c = n->a;
	u64 pos = 0;
	/* cache the last position to make sequential reads O(n) */
	if (n->b && off >= (n->b >> 32) * (u64)f->clus_bytes) {
		pos = (n->b >> 32) * (u64)f->clus_bytes;
		c = n->b & 0xffffffff;
	}
	while (c >= 2 && c < 0x0ffffff0 && pos + f->clus_bytes <= off) {
		c = next_clus(fs, c);
		pos += f->clus_bytes;
	}
	while (len) {
		if (c < 2 || c >= 0x0ffffff0) return -1;
		n->b = ((pos / f->clus_bytes) << 32) | c;
		u32 o = off - pos, k = MIN(len, f->clus_bytes - o);
		if (vol_read(&fs->vol, f->data_start + (u64)(c - 2) * f->clus_bytes + o, k, out) < 0) return -1;
		out += k; off += k; len -= k;
		c = next_clus(fs, c);
		pos += f->clus_bytes;
	}
	return 0;
}

static int fat_readdir(struct fs *fs, struct fnode *dir, dir_cb cb, void *arg)
{
	struct fat *f = fs->priv;
	u8 e[32];
	char lfn[256];
	int lfn_ok = 0;
	u32 limit = (dir->a == 0) ? f->root_bytes : 0x200000;
	struct fnode d = *dir;
	d.b = 0;
	for (u32 off = 0; off < limit; off += 32) {
		if (fat_read(fs, &d, off, 32, e) < 0) break;
		if (e[0] == 0) break;
		if (e[0] == 0xe5) { lfn_ok = 0; continue; }
		if (e[11] == 0x0f) {
			int seq = e[0] & 0x1f;
			if (e[0] & 0x40) { memset(lfn, 0, sizeof(lfn)); lfn_ok = 1; }
			static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
			for (int i = 0; i < 13; i++) {
				int idx = (seq - 1) * 13 + i;
				u16 ch = e[pos[i]] | (e[pos[i] + 1] << 8);
				if (idx < 255 && ch != 0xffff) lfn[idx] = ch && ch < 128 ? ch : (ch ? '_' : 0);
			}
			continue;
		}
		if (e[11] & 0x08) { lfn_ok = 0; continue; }	/* volume label */
		char name[256];
		if (lfn_ok && lfn[0]) {
			strlcpy(name, lfn, sizeof(name));
		} else {
			int k = 0;
			for (int i = 0; i < 8 && e[i] != ' '; i++) name[k++] = (e[12] & 8) ? tolower(e[i]) : e[i];
			if (e[8] != ' ') {
				name[k++] = '.';
				for (int i = 8; i < 11 && e[i] != ' '; i++) name[k++] = (e[12] & 16) ? tolower(e[i]) : e[i];
			}
			name[k] = 0;
		}
		lfn_ok = 0;
		if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
		struct fnode n;
		memset(&n, 0, sizeof(n));
		n.dir = (e[11] & 0x10) != 0;
		n.a = *(u16 *)(e + 26) | ((u32)*(u16 *)(e + 20) << 16);
		n.size = n.dir ? 0xffffffff : *(u32 *)(e + 28);
		cb(arg, name, &n);
	}
	return 0;
}

const struct fs_ops fat_ops = { "fat", fat_mount, fat_root, fat_readdir, fat_read, true };
