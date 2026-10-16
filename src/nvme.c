/* NVMe: one admin queue pair and one I/O queue pair, polled. */
#include "blockdev.h"
#include "pci.h"

#define QD 16

struct nvme {
	uintptr_t bar;
	u32 dstrd;
	u32 *asq, *acq, *iosq, *iocq;
	u16 asq_tail, acq_head, iosq_tail, iocq_head;
	u8 aphase, iophase;
	u16 cid;
	u32 nsid;
	u8 *bounce;	/* 64 KiB data bounce */
	u64 *prplist;
};

#define BOUNCE (64 * 1024)

static int submit(struct nvme *n, bool admin, u32 *cmd)
{
	u32 *sq = admin ? n->asq : n->iosq, *cq = admin ? n->acq : n->iocq;
	u16 *tail = admin ? &n->asq_tail : &n->iosq_tail, *head = admin ? &n->acq_head : &n->iocq_head;
	u8 *phase = admin ? &n->aphase : &n->iophase;
	int qid = admin ? 0 : 1;
	cmd[0] = (cmd[0] & 0xffff) | ((u32)(++n->cid) << 16);
	memcpy(sq + *tail * 16, cmd, 64);
	*tail = (*tail + 1) % QD;
	mb();
	write32(n->bar + 0x1000 + (2 * qid) * (4 << n->dstrd), *tail);
	u64 end = timer_us() + 5000000;
	for (;;) {
		volatile u32 *e = cq + *head * 4;
		if (((e[3] >> 16) & 1) == *phase) {
			u32 status = (e[3] >> 17) & 0x7fff;
			*head = (*head + 1) % QD;
			if (*head == 0) *phase ^= 1;
			write32(n->bar + 0x1000 + (2 * qid + 1) * (4 << n->dstrd), *head);
			return status ? -1 : 0;
		}
		if (timer_us() > end) return -1;
	}
}

static void set_prp(struct nvme *n, u32 *cmd, u32 bytes)
{
	cmd[6] = (uintptr_t)n->bounce; cmd[7] = 0;
	u32 pages = (bytes + 4095) / 4096;
	if (pages <= 1) { cmd[8] = cmd[9] = 0; }
	else if (pages == 2) { cmd[8] = (uintptr_t)n->bounce + 4096; cmd[9] = 0; }
	else {
		for (u32 i = 1; i < pages; i++) n->prplist[i - 1] = (uintptr_t)n->bounce + i * 4096;
		cmd[8] = (uintptr_t)n->prplist; cmd[9] = 0;
	}
}

static int nvme_read(struct blockdev *bd, u64 lba, u32 count, void *buf)
{
	struct nvme *n = bd->priv;
	u8 *out = buf;
	u32 per = BOUNCE / bd->sector_size;
	while (count) {
		u32 c = MIN(count, per);
		u32 cmd[16] = { 0 };
		cmd[0] = 0x02;
		cmd[1] = n->nsid;
		set_prp(n, cmd, c * bd->sector_size);
		cmd[10] = lba; cmd[11] = lba >> 32;
		cmd[12] = c - 1;
		if (submit(n, false, cmd) < 0) return -1;
		memcpy(out, n->bounce, c * bd->sector_size);
		out += c * bd->sector_size; lba += c; count -= c;
	}
	return 0;
}

static void nvme_init(struct pci_dev *pd)
{
	uintptr_t bar = pci_bar_mem(pd, 0);
	if (!bar) return;
	pci_enable(pd);
	struct nvme *n = zalloc(sizeof(*n));
	n->bar = bar;
	u64 cap = read32(bar) | ((u64)read32(bar + 4) << 32);
	n->dstrd = (cap >> 32) & 0xf;
	u32 to = ((cap >> 24) & 0xff) * 500 + 1000;
	write32(bar + 0x14, 0);	/* CC.EN = 0 */
	u64 end = timer_us() + (u64)to * 1000;
	while (read32(bar + 0x1c) & 1) if (timer_us() > end) return;
	n->asq = memalign(4096, 4096); n->acq = memalign(4096, 4096);
	n->iosq = memalign(4096, 4096); n->iocq = memalign(4096, 4096);
	n->bounce = memalign(4096, BOUNCE);
	n->prplist = memalign(4096, 4096);
	if (!n->asq || !n->acq || !n->iosq || !n->iocq || !n->bounce || !n->prplist) return;
	memset(n->asq, 0, 4096); memset(n->acq, 0, 4096);
	memset(n->iosq, 0, 4096); memset(n->iocq, 0, 4096);
	n->aphase = n->iophase = 1;
	write32(bar + 0x24, ((QD - 1) << 16) | (QD - 1));
	write32(bar + 0x28, (uintptr_t)n->asq); write32(bar + 0x2c, 0);
	write32(bar + 0x30, (uintptr_t)n->acq); write32(bar + 0x34, 0);
	write32(bar + 0x14, (4 << 20) | (6 << 16) | 1);
	end = timer_us() + (u64)to * 1000;
	while (!(read32(bar + 0x1c) & 1)) {
		if (read32(bar + 0x1c) & 2) return;
		if (timer_us() > end) return;
	}
	u32 cmd[16];
	/* identify controller for the model string */
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = 0x06; set_prp(n, cmd, 4096); cmd[10] = 1;
	char model[41] = "";
	if (submit(n, true, cmd) == 0) {
		memcpy(model, n->bounce + 24, 40);
		model[40] = 0;
		for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;
	}
	/* create I/O completion and submission queues */
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = 0x05; cmd[6] = (uintptr_t)n->iocq; cmd[10] = ((QD - 1) << 16) | 1; cmd[11] = 1;
	if (submit(n, true, cmd) < 0) return;
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = 0x01; cmd[6] = (uintptr_t)n->iosq; cmd[10] = ((QD - 1) << 16) | 1; cmd[11] = (1 << 16) | 1;
	if (submit(n, true, cmd) < 0) return;
	/* first active namespace (the I/O queue pair is not shared) */
	for (u32 ns = 1; ns <= 4; ns++) {
		memset(cmd, 0, sizeof(cmd));
		cmd[0] = 0x06; cmd[1] = ns; set_prp(n, cmd, 4096); cmd[10] = 0;
		if (submit(n, true, cmd) < 0) continue;
		u64 nsze = *(u64 *)n->bounce;
		if (!nsze) continue;
		u8 flbas = n->bounce[26] & 0xf;
		u32 lbaf = *(u32 *)(n->bounce + 128 + flbas * 4);
		u32 lbads = (lbaf >> 16) & 0xff;
		n->nsid = ns;
		struct blockdev *bd = bdev_register();
		if (!bd) return;
		bd->priv = n;
		bd->driver = "nvme";
		bd->type = BDEV_DISK;
		bd->sector_size = 1u << lbads;
		bd->nsectors = nsze;
		bd->read = nvme_read;
		strlcpy(bd->model, model, sizeof(bd->model));
		snprintf(bd->path, sizeof(bd->path), "%s/disk@%x", pd->path, ns);
		break;
	}
}

void nvme_probe(void)
{
	for (int i = 0; i < pci_ndevs; i++) {
		struct pci_dev *p = &pci_devs[i];
		if (p->class == 1 && p->subclass == 8) { p->claimed = true; nvme_init(p); }
	}
}
