/* Block device registry and a small read cache for filesystem metadata. */
#include "blockdev.h"
#include "console.h"

struct blockdev *bdevs[MAX_BDEV];
int nbdevs;

struct blockdev *bdev_register(void)
{
	if (nbdevs >= MAX_BDEV) return NULL;
	struct blockdev *bd = zalloc(sizeof(*bd));
	bdevs[nbdevs++] = bd;
	return bd;
}

static void assign_aliases(void)
{
	int ndisk = 0, ncd = 0;
	for (int i = 0; i < nbdevs; i++) {
		struct blockdev *bd = bdevs[i];
		if (bd->type == BDEV_CDROM) snprintf(bd->alias, sizeof(bd->alias), "cdrom%d", ncd++);
		else snprintf(bd->alias, sizeof(bd->alias), "disk%d", ndisk++);
	}
}

void bdev_probe_all(void)
{
	nvme_probe();
	ahci_probe();
	ata_probe();
	assign_aliases();
}

struct blockdev *bdev_find(const char *name)
{
	for (int i = 0; i < nbdevs; i++)
		if (!strcmp(bdevs[i]->alias, name) || !strcmp(bdevs[i]->path, name))
			return bdevs[i];
	return NULL;
}

/* ---- cache: 32 x 4 KiB lines, LRU-ish ---- */
#define CLINE 4096
#define NCACHE 64
struct cline { struct blockdev *bd; u64 off; u32 age; bool valid; u8 *data; };
static struct cline cache[NCACHE];
static u32 cache_clock;

int bdev_read(struct blockdev *bd, u64 lba, u32 count, void *buf)
{
	u8 *p = buf;
	while (count) {
		u32 n = MIN(count, 64u);
		int tries = 3, r;
		while ((r = bd->read(bd, lba, n, p)) < 0 && --tries)
			;
		if (r < 0) return r;
		lba += n; count -= n; p += n * bd->sector_size;
	}
	return 0;
}

static u8 *cached_line(struct blockdev *bd, u64 off)
{
	struct cline *victim = &cache[0];
	for (int i = 0; i < NCACHE; i++) {
		struct cline *c = &cache[i];
		if (c->valid && c->bd == bd && c->off == off) { c->age = ++cache_clock; return c->data; }
		if (!c->valid || c->age < victim->age) victim = c;
	}
	if (!victim->data && !(victim->data = memalign(64, CLINE))) return NULL;
	victim->valid = false;
	u32 ss = bd->sector_size;
	u64 lba = off / ss;
	u32 cnt = CLINE / ss;
	if (lba >= bd->nsectors) return NULL;
	if (lba + cnt > bd->nsectors) {
		memset(victim->data, 0, CLINE);
		cnt = bd->nsectors - lba;
	}
	if (bdev_read(bd, lba, cnt, victim->data) < 0) return NULL;
	victim->bd = bd; victim->off = off; victim->valid = true; victim->age = ++cache_clock;
	return victim->data;
}

int vol_read(struct volume *v, u64 off, u32 len, void *buf)
{
	struct blockdev *bd = v->bd;
	u8 *out = buf;
	if (off + len > v->size) return -1;
	off += v->start;
	u32 ss = bd->sector_size;
	while (len) {
		/* Large aligned reads bypass the cache. */
		if ((off % ss) == 0 && len >= 16 * CLINE) {
			u32 secs = MIN(len / ss, 2048u);
			if (bdev_read(bd, off / ss, secs, out) < 0) return -1;
			off += secs * ss; out += secs * ss; len -= secs * ss;
			continue;
		}
		u64 line = off & ~(u64)(CLINE - 1);
		u8 *d = cached_line(bd, line);
		if (!d) return -1;
		u32 o = off - line, n = MIN(len, CLINE - o);
		memcpy(out, d + o, n);
		off += n; out += n; len -= n;
	}
	return 0;
}
