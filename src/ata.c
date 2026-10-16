/* Parallel ATA / ATAPI in PIO mode: legacy and PCI-native IDE channels. */
#include "blockdev.h"
#include "pci.h"

struct ata_chan { u16 cmd, ctl; };
struct ata_drive { struct ata_chan ch; int slave; bool lba48; bool atapi; };

#define REG_DATA 0
#define REG_ERR 1
#define REG_COUNT 2
#define REG_LBA0 3
#define REG_LBA1 4
#define REG_LBA2 5
#define REG_DEV 6
#define REG_CMD 7
#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF 0x20
#define ST_BSY 0x80

static int wait_notbusy(struct ata_chan *c, u32 ms)
{
	u64 end = timer_us() + (u64)ms * 1000;
	for (;;) {
		u8 s = inb(c->cmd + REG_CMD);
		if (s == 0xff) return -1;
		if (!(s & ST_BSY)) return s;
		if (timer_us() > end) return -1;
	}
}

static int wait_drq(struct ata_chan *c, u32 ms)
{
	u64 end = timer_us() + (u64)ms * 1000;
	for (;;) {
		u8 s = inb(c->cmd + REG_CMD);
		if (!(s & ST_BSY)) {
			if (s & (ST_ERR | ST_DF)) return -1;
			if (s & ST_DRQ) return 0;
		}
		if (timer_us() > end) return -1;
	}
}

static void select_drive(struct ata_drive *d, u8 extra)
{
	outb(d->ch.cmd + REG_DEV, 0xa0 | (d->slave << 4) | extra);
	for (int i = 0; i < 4; i++) inb(d->ch.ctl);	/* 400ns */
}

static int ata_read(struct blockdev *bd, u64 lba, u32 count, void *buf)
{
	struct ata_drive *d = bd->priv;
	u16 *p = buf;
	while (count) {
		u32 n = MIN(count, 128u);
		if (wait_notbusy(&d->ch, 5000) < 0) return -1;
		if (d->lba48) {
			select_drive(d, 0x40);
			outb(d->ch.cmd + REG_COUNT, n >> 8);
			outb(d->ch.cmd + REG_LBA0, lba >> 24);
			outb(d->ch.cmd + REG_LBA1, lba >> 32);
			outb(d->ch.cmd + REG_LBA2, lba >> 40);
			outb(d->ch.cmd + REG_COUNT, n);
			outb(d->ch.cmd + REG_LBA0, lba);
			outb(d->ch.cmd + REG_LBA1, lba >> 8);
			outb(d->ch.cmd + REG_LBA2, lba >> 16);
			outb(d->ch.cmd + REG_CMD, 0x24);
		} else {
			select_drive(d, 0x40 | ((lba >> 24) & 0xf));
			outb(d->ch.cmd + REG_COUNT, n & 0xff);
			outb(d->ch.cmd + REG_LBA0, lba);
			outb(d->ch.cmd + REG_LBA1, lba >> 8);
			outb(d->ch.cmd + REG_LBA2, lba >> 16);
			outb(d->ch.cmd + REG_CMD, 0x20);
		}
		for (u32 s = 0; s < n; s++) {
			if (wait_drq(&d->ch, 10000) < 0) return -1;
			__asm__ volatile("rep insw" : "+D"(p) : "d"(d->ch.cmd), "c"(256) : "memory");
		}
		lba += n; count -= n;
	}
	return 0;
}

static int atapi_packet(struct ata_drive *d, const u8 *pkt, void *buf, u32 len)
{
	if (wait_notbusy(&d->ch, 5000) < 0) return -1;
	select_drive(d, 0);
	outb(d->ch.cmd + REG_ERR, 0);		/* PIO */
	outb(d->ch.cmd + REG_LBA1, 0xfe);
	outb(d->ch.cmd + REG_LBA2, 0xff);
	outb(d->ch.cmd + REG_CMD, 0xa0);
	if (wait_drq(&d->ch, 5000) < 0) return -1;
	__asm__ volatile("rep outsw" : : "S"(pkt), "d"(d->ch.cmd), "c"(6) : "memory");
	u8 *p = buf;
	u32 got = 0;
	for (;;) {
		int s = wait_notbusy(&d->ch, 20000);
		if (s < 0) return -1;
		if (s & (ST_ERR | ST_DF)) return -1;
		if (!(s & ST_DRQ)) break;
		u32 bc = inb(d->ch.cmd + REG_LBA1) | (inb(d->ch.cmd + REG_LBA2) << 8);
		for (u32 i = 0; i < bc; i += 2) {
			u16 w = inw(d->ch.cmd);
			if (got + 1 < len) { p[got] = w; p[got + 1] = w >> 8; }
			else if (got < len) p[got] = w;
			got += 2;
		}
	}
	return got;
}

static int atapi_read(struct blockdev *bd, u64 lba, u32 count, void *buf)
{
	struct ata_drive *d = bd->priv;
	u8 *p = buf;
	while (count) {
		u32 n = MIN(count, 16u);
		u8 pkt[12] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, n >> 8, n, 0, 0, 0 };
		int r = atapi_packet(d, pkt, p, n * 2048);
		if (r < (int)(n * 2048)) {
			/* clear a pending UNIT ATTENTION and retry once */
			u8 sense[18], rs[12] = { 0x03, 0, 0, 0, 18, 0 };
			atapi_packet(d, rs, sense, sizeof(sense));
			r = atapi_packet(d, pkt, p, n * 2048);
			if (r < (int)(n * 2048)) return -1;
		}
		lba += n; count -= n; p += n * 2048;
	}
	return 0;
}

static void model_string(char *out, const u16 *id)
{
	for (int i = 0; i < 20; i++) {
		out[i * 2] = id[27 + i] >> 8;
		out[i * 2 + 1] = id[27 + i] & 0xff;
	}
	out[40] = 0;
	for (int i = 39; i >= 0 && out[i] == ' '; i--) out[i] = 0;
}

static void probe_drive(struct ata_chan ch, int slave, const char *ctrl_path, int chan_no)
{
	struct ata_drive tmp = { ch, slave, false, false };
	select_drive(&tmp, 0);
	u8 st = inb(ch.cmd + REG_CMD);
	if (st == 0xff || st == 0x7f) return;
	outb(ch.cmd + REG_COUNT, 0x55);
	outb(ch.cmd + REG_LBA0, 0xaa);
	if (inb(ch.cmd + REG_COUNT) != 0x55 || inb(ch.cmd + REG_LBA0) != 0xaa) return;
	/* soft reset only once per channel, from the master probe */
	if (!slave) {
		outb(ch.ctl, 0x06); udelay(10); outb(ch.ctl, 0x02); mdelay(5);
		wait_notbusy(&ch, 10000);
		select_drive(&tmp, 0);
	}
	u16 id[256];
	outb(ch.cmd + REG_CMD, 0xec);
	mdelay(1);
	st = inb(ch.cmd + REG_CMD);
	if (st == 0) return;
	int s = wait_notbusy(&ch, 10000);
	if (s < 0) return;
	u8 l1 = inb(ch.cmd + REG_LBA1), l2 = inb(ch.cmd + REG_LBA2);
	bool atapi = false;
	if ((l1 == 0x14 && l2 == 0xeb) || (l1 == 0x69 && l2 == 0x96)) {
		atapi = true;
		outb(ch.cmd + REG_CMD, 0xa1);
		mdelay(1);
		if (wait_notbusy(&ch, 10000) < 0) return;
	} else if (l1 || l2) {
		return;	/* SATA signature on an IDE-mode port, etc. */
	}
	if (wait_drq(&ch, 3000) < 0) return;
	for (int i = 0; i < 256; i++) id[i] = inw(ch.cmd);

	struct ata_drive *d = zalloc(sizeof(*d));
	*d = tmp;
	d->atapi = atapi;
	struct blockdev *bd = bdev_register();
	if (!bd) return;
	bd->priv = d;
	bd->driver = "ata";
	model_string(bd->model, id);
	if (atapi) {
		bd->type = BDEV_CDROM;
		bd->sector_size = 2048;
		bd->read = atapi_read;
		bd->nsectors = 0xffffffff;
		u8 cap[8], pkt[12] = { 0x25 };
		for (int tries = 0; tries < 3; tries++) {
			if (atapi_packet(d, pkt, cap, 8) >= 8) {
				bd->nsectors = (((u32)cap[0] << 24) | (cap[1] << 16) | (cap[2] << 8) | cap[3]) + 1;
				break;
			}
			mdelay(10);
		}
		snprintf(bd->path, sizeof(bd->path), "%s/ide@%d/cdrom@%d,0", ctrl_path, chan_no, slave);
	} else {
		bd->type = BDEV_DISK;
		bd->sector_size = 512;
		bd->read = ata_read;
		d->lba48 = (id[83] & (1 << 10)) != 0;
		if (d->lba48) memcpy(&bd->nsectors, &id[100], 8);
		else bd->nsectors = id[60] | ((u32)id[61] << 16);
		snprintf(bd->path, sizeof(bd->path), "%s/ide@%d/disk@%d,0", ctrl_path, chan_no, slave);
	}
}

static bool legacy_done[2];

static void probe_channel(struct ata_chan ch, const char *path, int no)
{
	if (inb(ch.cmd + REG_CMD) == 0xff) return;
	outb(ch.ctl, 0x02);	/* nIEN */
	probe_drive(ch, 0, path, no);
	probe_drive(ch, 1, path, no);
}

void ata_probe(void)
{
	for (int i = 0; i < pci_ndevs; i++) {
		struct pci_dev *p = &pci_devs[i];
		if (p->class != 1 || p->subclass != 1) continue;
		p->claimed = true;
		pci_enable(p);
		for (int c = 0; c < 2; c++) {
			struct ata_chan ch;
			if (p->progif & (1 << (c * 2))) {
				ch.cmd = pci_bar_io(p, c * 2);
				ch.ctl = pci_bar_io(p, c * 2 + 1) + 2;
				if (!ch.cmd) continue;
			} else {
				if (legacy_done[c]) continue;
				legacy_done[c] = true;
				ch.cmd = c ? 0x170 : 0x1f0;
				ch.ctl = c ? 0x376 : 0x3f6;
			}
			probe_channel(ch, p->path, c);
		}
	}
	/* No PCI IDE function found: try the ISA locations anyway. */
	if (!legacy_done[0]) {
		legacy_done[0] = true;
		probe_channel((struct ata_chan){ 0x1f0, 0x3f6 }, "/isa/ide", 0);
	}
	if (!legacy_done[1]) {
		legacy_done[1] = true;
		probe_channel((struct ata_chan){ 0x170, 0x376 }, "/isa/ide", 1);
	}
}
