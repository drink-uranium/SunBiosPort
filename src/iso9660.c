/* ISO 9660 with Rock Ridge (names, symlinks) and Joliet fallback names. */
#include "fs.h"

struct iso { u32 root_lba, root_len; bool rr; u32 susp_skip; bool joliet; };

static int iso_mount(struct fs *fs)
{
	u8 vd[2048];
	struct iso *iso = zalloc(sizeof(*iso));
	bool found = false;
	for (u32 s = 16; s < 32; s++) {
		if (vol_read(&fs->vol, (u64)s * 2048, 2048, vd) < 0) break;
		if (memcmp(vd + 1, "CD001", 5)) break;
		if (vd[0] == 1 && !found) {
			iso->root_lba = *(u32 *)(vd + 156 + 2);
			iso->root_len = *(u32 *)(vd + 156 + 10);
			found = true;
		} else if (vd[0] == 2 && vd[88] == 0x25 && vd[89] == 0x2f &&
			   (vd[90] == 0x40 || vd[90] == 0x43 || vd[90] == 0x45)) {
			/* remember Joliet; used only when Rock Ridge is absent */
			iso->joliet = true;
			fs->priv = (void *)(uintptr_t)(*(u32 *)(vd + 156 + 2));
		} else if (vd[0] == 255) {
			break;
		}
	}
	if (!found) { free(iso); return -1; }
	/* Rock Ridge: look for SUSP "SP" in the root's "." entry */
	u8 sec[2048];
	if (vol_read(&fs->vol, (u64)iso->root_lba * 2048, 2048, sec) == 0) {
		u8 *r = sec;
		u32 su = 33 + r[32] + ((r[32] & 1) ? 0 : 1);
		if (su + 7 <= r[0] && r[su] == 'S' && r[su + 1] == 'P' && r[su + 4] == 0xbe && r[su + 5] == 0xef) {
			iso->rr = true;
			iso->susp_skip = r[su + 6];
		}
	}
	if (!iso->rr && iso->joliet) {
		u32 jl = (u32)(uintptr_t)fs->priv;
		u8 j[2048];
		for (u32 s = 16; s < 32; s++) {
			if (vol_read(&fs->vol, (u64)s * 2048, 2048, j) < 0) break;
			if (j[0] == 2 && *(u32 *)(j + 156 + 2) == jl) {
				iso->root_lba = jl;
				iso->root_len = *(u32 *)(j + 156 + 10);
				break;
			}
		}
	} else {
		iso->joliet = false;
	}
	fs->priv = iso;
	fs->ci = !iso->rr;
	return 0;
}

static int iso_root(struct fs *fs, struct fnode *n)
{
	struct iso *iso = fs->priv;
	memset(n, 0, sizeof(*n));
	n->dir = true;
	n->a = iso->root_lba;
	n->size = iso->root_len;
	return 0;
}

/* Parse SUSP entries of one directory record, following CE continuations. */
static void rr_parse(struct fs *fs, u8 *su, int len, char *name, int *has_nm, struct fnode *n)
{
	int depth = 0;
	u8 cebuf[2048];
	while (len >= 4 && depth < 4) {
		u8 *p = su;
		int remaining = len;
		u32 ce_lba = 0, ce_off = 0, ce_len = 0;
		while (remaining >= 4) {
			int l = p[2];
			if (l < 4 || l > remaining) break;
			if (p[0] == 'N' && p[1] == 'M' && l >= 5) {
				int nl = l - 5;
				int cur = *has_nm ? (int)strlen(name) : 0;
				if (cur + nl > 255) nl = 255 - cur;
				if (!(p[4] & 6)) {
					memcpy(name + cur, p + 5, nl);
					name[cur + nl] = 0;
					*has_nm = 1;
				}
			} else if (p[0] == 'S' && p[1] == 'L' && l >= 5) {
				n->link = true;
				/* build the link target into raw[] */
				char *t = (char *)n->raw;
				int tl = strlen(t);
				u8 *c = p + 5, *ce = p + l;
				while (c + 2 <= ce) {
					u8 fl = c[0], cl = c[1];
					if (tl && t[tl - 1] != '/' && tl < 150) t[tl++] = '/';
					if (fl & 2) { if (tl < 150) t[tl++] = '.'; }
					else if (fl & 4) { if (tl < 149) { t[tl++] = '.'; t[tl++] = '.'; } }
					else if (fl & 8) { tl = 0; if (tl < 150) t[tl++] = '/'; }
					else for (int k = 0; k < cl && tl < 150; k++) t[tl++] = c[2 + k];
					c += 2 + cl;
				}
				t[tl] = 0;
				n->size = tl;
			} else if (p[0] == 'C' && p[1] == 'E' && l >= 28) {
				ce_lba = *(u32 *)(p + 4);
				ce_off = *(u32 *)(p + 12);
				ce_len = *(u32 *)(p + 20);
			} else if (p[0] == 'S' && p[1] == 'T') {
				break;
			}
			p += l;
			remaining -= l;
		}
		if (!ce_len || ce_off + ce_len > 2048) break;
		if (vol_read(&fs->vol, (u64)ce_lba * 2048, 2048, cebuf) < 0) break;
		su = cebuf + ce_off;
		len = ce_len;
		depth++;
	}
}

static int iso_readdir(struct fs *fs, struct fnode *dir, dir_cb cb, void *arg)
{
	struct iso *iso = fs->priv;
	u8 sec[2048];
	for (u32 off = 0; off < dir->size; off += 2048) {
		if (vol_read(&fs->vol, dir->a * 2048 + off, 2048, sec) < 0) return -1;
		for (u32 i = 0; i < 2048;) {
			u8 *r = sec + i;
			if (r[0] == 0) break;
			if (i + r[0] > 2048) break;
			u8 nl = r[32];
			if (nl == 1 && (r[33] == 0 || r[33] == 1)) { i += r[0]; continue; }
			struct fnode n;
			memset(&n, 0, sizeof(n));
			n.a = *(u32 *)(r + 2);
			n.size = *(u32 *)(r + 10);
			n.dir = (r[25] & 2) != 0;
			char name[256];
			int has_nm = 0;
			if (iso->rr) {
				int su = 33 + nl + ((nl & 1) ? 0 : 1) + iso->susp_skip;
				struct fnode tmp = n;
				tmp.raw[0] = 0;
				rr_parse(fs, r + su, r[0] - su, name, &has_nm, &tmp);
				if (tmp.link) { n.link = true; memcpy(n.raw, tmp.raw, sizeof(n.raw)); n.size = tmp.size; }
			}
			if (!has_nm) {
				int k = 0;
				if (iso->joliet) {
					for (int j = 0; j + 1 < nl && k < 255; j += 2) {
						u16 c = (r[33 + j] << 8) | r[34 + j];
						name[k++] = c < 128 ? c : '_';
					}
				} else {
					for (int j = 0; j < nl && k < 255; j++) name[k++] = r[33 + j];
				}
				name[k] = 0;
				char *semi = strchr(name, ';');
				if (semi) *semi = 0;
				size_t l = strlen(name);
				if (l && name[l - 1] == '.') name[l - 1] = 0;
				if (!iso->joliet)
					for (char *c = name; *c; c++) *c = tolower(*c);
			}
			cb(arg, name, &n);
			i += r[0];
		}
	}
	return 0;
}

static int iso_read(struct fs *fs, struct fnode *f, u64 off, u32 len, void *buf)
{
	if (f->link) {
		u32 l = MIN(len, (u32)f->size);
		memcpy(buf, (char *)f->raw + off, l);
		return 0;
	}
	return vol_read(&fs->vol, f->a * 2048 + off, len, buf);
}

const struct fs_ops iso9660_ops = { "iso9660", iso_mount, iso_root, iso_readdir, iso_read, false };
