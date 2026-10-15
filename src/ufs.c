/* UFS as written by Solaris (and 4.4BSD UFS1, which shares the layout):
 * superblock at 8 KiB, 128-byte dinodes, direct + 3 indirect blocks. */
#include "fs.h"

struct ufs {
	u32 bsize, fsize, frag, ipg, fpg, iblkno, cgoffset, cgmask, inopb, nindir;
};

static int ufs_mount(struct fs *fs)
{
	u8 sb[1376];
	if (vol_read(&fs->vol, 8192, sizeof(sb), sb) < 0) return -1;
	if (*(u32 *)(sb + 1372) != 0x011954) return -1;
	struct ufs *u = zalloc(sizeof(*u));
	u->iblkno = *(u32 *)(sb + 16);
	u->cgoffset = *(u32 *)(sb + 24);
	u->cgmask = *(u32 *)(sb + 28);
	u->bsize = *(u32 *)(sb + 48);
	u->fsize = *(u32 *)(sb + 52);
	u->frag = *(u32 *)(sb + 56);
	u->nindir = *(u32 *)(sb + 116);
	u->inopb = *(u32 *)(sb + 120);
	u->ipg = *(u32 *)(sb + 184);
	u->fpg = *(u32 *)(sb + 188);
	if (!u->bsize || !u->fsize || !u->ipg || !u->inopb || u->bsize > 65536) { free(u); return -1; }
	fs->priv = u;
	return 0;
}

static int read_inode(struct fs *fs, u32 ino, struct fnode *n)
{
	struct ufs *u = fs->priv;
	u32 cg = ino / u->ipg;
	u64 cgstart = (u64)u->fpg * cg + (u64)u->cgoffset * (cg & ~u->cgmask);
	u64 fsba = cgstart + u->iblkno + ((ino % u->ipg) / u->inopb) * u->frag;
	u64 off = fsba * u->fsize + (ino % u->inopb) * 128;
	memset(n, 0, sizeof(*n));
	if (vol_read(&fs->vol, off, 128, n->raw) < 0) return -1;
	u16 mode = *(u16 *)n->raw;
	n->size = *(u64 *)(n->raw + 8);
	n->dir = (mode & 0xf000) == 0x4000;
	n->link = (mode & 0xf000) == 0xa000;
	n->a = ino;
	return 0;
}

static int ufs_root(struct fs *fs, struct fnode *n) { return read_inode(fs, 2, n); }

static u32 bmap(struct fs *fs, struct fnode *n, u64 lb)
{
	struct ufs *u = fs->priv;
	u32 *db = (u32 *)(n->raw + 40), *ib = (u32 *)(n->raw + 88);
	if (lb < 12) return db[lb];
	lb -= 12;
	u64 span = 1;
	for (int level = 0; level < 3; level++) {
		span *= u->nindir;
		if (lb < span) {
			u32 blk = ib[level];
			for (int l = level; l >= 0 && blk; l--) {
				u64 div = 1;
				for (int k = 0; k < l; k++) div *= u->nindir;
				u32 idx = (lb / div) % u->nindir;
				if (vol_read(&fs->vol, (u64)blk * u->fsize + idx * 4, 4, &blk) < 0) return 0;
			}
			return blk;
		}
		lb -= span;
	}
	return 0;
}

static int ufs_read(struct fs *fs, struct fnode *n, u64 off, u32 len, void *buf)
{
	struct ufs *u = fs->priv;
	u8 *out = buf;
	u32 blocks = *(u32 *)(n->raw + 104);
	if (n->link && n->size < 60 && blocks == 0) {	/* fast symlink */
		memcpy(buf, n->raw + 40 + off, len);
		return 0;
	}
	while (len) {
		u64 lb = off / u->bsize;
		u32 o = off % u->bsize, k = MIN(len, u->bsize - o);
		u32 fb = bmap(fs, n, lb);
		while (fb && k < len && k < 8u * 1024 * 1024 &&
		       bmap(fs, n, lb + (o + k) / u->bsize) == fb + ((o + k) / u->bsize) * u->frag)
			k += MIN(len - k, u->bsize);
		if (!fb) memset(out, 0, k);
		else if (vol_read(&fs->vol, (u64)fb * u->fsize + o, k, out) < 0) return -1;
		out += k; off += k; len -= k;
	}
	return 0;
}

static int ufs_readdir(struct fs *fs, struct fnode *dir, dir_cb cb, void *arg)
{
	u8 blk[512];
	for (u64 off = 0; off < dir->size; off += 512) {
		u32 l = MIN(512u, (u32)(dir->size - off));
		if (ufs_read(fs, dir, off, l, blk) < 0) return -1;
		for (u32 i = 0; i + 8 <= l;) {
			u32 ino = *(u32 *)(blk + i);
			u16 rl = *(u16 *)(blk + i + 4);
			/* Solaris: u16 d_namlen. 4.4BSD: u8 d_type, u8 d_namlen. */
			u32 nl = blk[i + 7] ? blk[i + 7] : blk[i + 6];
			if (rl < 8 || i + rl > l) break;
			if (ino && nl && 8 + nl <= rl) {
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
	return 0;
}

const struct fs_ops ufs_ops = { "ufs", ufs_mount, ufs_root, ufs_readdir, ufs_read, false };
