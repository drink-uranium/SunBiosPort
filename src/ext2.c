/* ext2/ext3/ext4 (read-only): block maps, extents, 64-bit descriptors. */
#include "fs.h"

struct ext2 {
	u32 bsize, inode_size, ipg, first_data, desc_size;
	u64 gdt;
	u32 incompat;
};

static int ext_mount(struct fs *fs)
{
	u8 sb[1024];
	if (vol_read(&fs->vol, 1024, 1024, sb) < 0) return -1;
	if (*(u16 *)(sb + 56) != 0xef53) return -1;
	struct ext2 *e = zalloc(sizeof(*e));
	e->bsize = 1024u << *(u32 *)(sb + 24);
	e->ipg = *(u32 *)(sb + 40);
	e->first_data = *(u32 *)(sb + 20);
	u32 rev = *(u32 *)(sb + 76);
	e->inode_size = rev ? *(u16 *)(sb + 88) : 128;
	e->incompat = *(u32 *)(sb + 96);
	e->desc_size = (e->incompat & 0x80) ? *(u16 *)(sb + 254) : 32;
	if (e->desc_size < 32) e->desc_size = 32;
	e->gdt = (u64)(e->first_data + 1) * e->bsize;
	fs->priv = e;
	return 0;
}

static int read_inode(struct fs *fs, u32 ino, struct fnode *n)
{
	struct ext2 *e = fs->priv;
	u32 g = (ino - 1) / e->ipg, idx = (ino - 1) % e->ipg;
	u8 d[64];
	if (vol_read(&fs->vol, e->gdt + (u64)g * e->desc_size, MIN(e->desc_size, 64u), d) < 0) return -1;
	u64 tbl = *(u32 *)(d + 8);
	if (e->desc_size >= 64) tbl |= (u64)*(u32 *)(d + 0x28) << 32;
	memset(n, 0, sizeof(*n));
	if (vol_read(&fs->vol, tbl * e->bsize + (u64)idx * e->inode_size, 128, n->raw) < 0) return -1;
	u16 mode = *(u16 *)n->raw;
	n->size = *(u32 *)(n->raw + 4) | ((u64)*(u32 *)(n->raw + 108) << 32);
	n->dir = (mode & 0xf000) == 0x4000;
	n->link = (mode & 0xf000) == 0xa000;
	n->a = ino;
	return 0;
}

static int ext_root(struct fs *fs, struct fnode *n) { return read_inode(fs, 2, n); }

/* logical block -> physical block (0 = hole) */
static u64 bmap(struct fs *fs, struct fnode *n, u64 lb)
{
	struct ext2 *e = fs->priv;
	u32 flags = *(u32 *)(n->raw + 32);
	u8 *iblock = n->raw + 40;
	if (flags & 0x80000) {	/* extents */
		u8 node[4096];
		u8 *h = iblock;
		int depth_guard = 0;
		for (;;) {
			if (*(u16 *)h != 0xf30a) return 0;
			u16 entries = *(u16 *)(h + 2), depth = *(u16 *)(h + 6);
			u8 *ent = h + 12;
			if (depth == 0) {
				for (int i = 0; i < entries; i++, ent += 12) {
					u32 first = *(u32 *)ent;
					u16 len = *(u16 *)(ent + 4);
					if (len > 32768) len -= 32768;	/* uninitialised */
					if (lb >= first && lb < (u64)first + len) {
						u64 start = *(u32 *)(ent + 8) | ((u64)*(u16 *)(ent + 6) << 32);
						return start + (lb - first);
					}
				}
				return 0;
			}
			u8 *pick = NULL;
			for (int i = 0; i < entries; i++, ent += 12)
				if (*(u32 *)ent <= lb) pick = ent;
			if (!pick || ++depth_guard > 5 || e->bsize > sizeof(node)) return 0;
			u64 leaf = *(u32 *)(pick + 4) | ((u64)*(u16 *)(pick + 8) << 32);
			if (vol_read(&fs->vol, leaf * e->bsize, e->bsize, node) < 0) return 0;
			h = node;
		}
	}
	u32 per = e->bsize / 4;
	if (lb < 12) return ((u32 *)iblock)[lb];
	lb -= 12;
	u32 blk;
	int levels;
	if (lb < per) { blk = ((u32 *)iblock)[12]; levels = 1; }
	else if ((lb -= per) < (u64)per * per) { blk = ((u32 *)iblock)[13]; levels = 2; }
	else { lb -= (u64)per * per; blk = ((u32 *)iblock)[14]; levels = 3; }
	for (int l = levels - 1; l >= 0 && blk; l--) {
		u64 div = 1;
		for (int k = 0; k < l; k++) div *= per;
		u32 idx = (lb / div) % per;
		if (vol_read(&fs->vol, (u64)blk * e->bsize + idx * 4, 4, &blk) < 0) return 0;
	}
	return blk;
}

static int ext_read(struct fs *fs, struct fnode *n, u64 off, u32 len, void *buf)
{
	struct ext2 *e = fs->priv;
	u8 *out = buf;
	if (n->link && n->size < 60 && !(*(u32 *)(n->raw + 32) & 0x80000)) {
		memcpy(buf, n->raw + 40 + off, len);
		return 0;
	}
	while (len) {
		u64 lb = off / e->bsize;
		u32 o = off % e->bsize;
		/* coalesce physically contiguous blocks into one request */
		u64 pb = bmap(fs, n, lb);
		u32 k = MIN(len, e->bsize - o);
		while (pb && k < len && k < 8u * 1024 * 1024 && bmap(fs, n, lb + (o + k) / e->bsize) == pb + (o + k) / e->bsize)
			k += MIN(len - k, e->bsize);
		if (!pb) memset(out, 0, k);
		else if (vol_read(&fs->vol, pb * e->bsize + o, k, out) < 0) return -1;
		out += k; off += k; len -= k;
	}
	return 0;
}

static int ext_readdir(struct fs *fs, struct fnode *dir, dir_cb cb, void *arg)
{
	struct ext2 *e = fs->priv;
	u8 *blk = malloc(e->bsize);
	if (!blk) return -1;
	for (u64 off = 0; off < dir->size; off += e->bsize) {
		if (ext_read(fs, dir, off, e->bsize, blk) < 0) break;
		for (u32 i = 0; i + 8 <= e->bsize;) {
			u32 ino = *(u32 *)(blk + i);
			u16 rl = *(u16 *)(blk + i + 4);
			u8 nl = blk[i + 6];
			if (rl < 8 || i + rl > e->bsize) break;
			if (ino && nl && i + 8 + nl <= e->bsize) {
				char name[256];
				memcpy(name, blk + i + 8, nl);
				name[nl] = 0;
				if (strcmp(name, ".") && strcmp(name, "..")) {
					struct fnode n;
					if (read_inode(fs, ino, &n) == 0) cb(arg, name, &n);
				}
			}
			i += rl;
		}
	}
	free(blk);
	return 0;
}

const struct fs_ops ext2_ops = { "ext2", ext_mount, ext_root, ext_readdir, ext_read, false };
