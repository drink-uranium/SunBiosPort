/* AHCI SATA host controllers: disks (READ DMA EXT) and ATAPI (PACKET). */
#include "blockdev.h"
#include "pci.h"

#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_PI 0x0c
#define P_CLB 0x00
#define P_FB 0x08
#define P_IS 0x10
#define P_CMD 0x18
#define P_TFD 0x20
#define P_SIG 0x24
#define P_SSTS 0x28
#define P_SERR 0x30
#define P_CI 0x38

struct ahci_port {
	uintptr_t base;
	u32 *clb;	/* command list, 32 headers */
	u8 *fis;
	u8 *tbl;	/* one command table: 0x80 header + PRDT */
	u8 *bounce;
	bool atapi;
};

#define BOUNCE (64 * 1024)
#define NPRD 16

static int port_stop(uintptr_t pb)
{
	u32 cmd = read32(pb + P_CMD);
	write32(pb + P_CMD, cmd & ~0x11u);	/* ST, FRE */
	u64 end = timer_us() + 500000;
	while (read32(pb + P_CMD) & 0xc000)	/* CR, FR */
		if (timer_us() > end) return -1;
	return 0;
}

static int ahci_cmd(struct ahci_port *p, u8 cmd, u64 lba, u32 count, const u8 *pkt, u32 bytes)
{
	uintptr_t pb = p->base;
	u64 end = timer_us() + 5000000;
	while (read32(pb + P_TFD) & 0x88)
		if (timer_us() > end) return -1;
	memset(p->tbl, 0, 0x80 + NPRD * 16);
	u8 *cfis = p->tbl;
	cfis[0] = 0x27;
	cfis[1] = 0x80;
	cfis[2] = cmd;
	cfis[3] = pkt ? 1 : 0;	/* features: DMA for PACKET */
	cfis[4] = lba; cfis[5] = lba >> 8; cfis[6] = lba >> 16;
	cfis[7] = 0x40;
	cfis[8] = lba >> 24; cfis[9] = lba >> 32; cfis[10] = lba >> 40;
	cfis[12] = count; cfis[13] = count >> 8;
	if (pkt) memcpy(p->tbl + 0x40, pkt, 12);
	u32 nprd = 0;
	for (u32 off = 0; off < bytes; off += 0x2000, nprd++) {
		u32 *prd = (u32 *)(p->tbl + 0x80 + nprd * 16);
		prd[0] = (uintptr_t)p->bounce + off;
		prd[1] = 0;
		prd[3] = MIN(0x2000u, bytes - off) - 1;
	}
	p->clb[0] = 5 | (pkt ? (1 << 5) : 0) | (nprd << 16);
	p->clb[1] = 0;
	p->clb[2] = (uintptr_t)p->tbl;
	p->clb[3] = 0;
	write32(pb + P_IS, 0xffffffff);
	mb();
	write32(pb + P_CI, 1);
	end = timer_us() + 10000000;
	while (read32(pb + P_CI) & 1) {
		if (read32(pb + P_IS) & (1 << 30)) return -1;	/* TFES */
		if (timer_us() > end) return -1;
	}
	if (read32(pb + P_TFD) & 1) return -1;
	return 0;
}

static int ahci_read(struct blockdev *bd, u64 lba, u32 count, void *buf)
{
	struct ahci_port *p = bd->priv;
	u8 *out = buf;
	u32 per = BOUNCE / bd->sector_size;
	while (count) {
		u32 n = MIN(count, per);
		int r;
		if (p->atapi) {
			u8 pkt[12] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, n >> 8, n, 0, 0, 0 };
			r = ahci_cmd(p, 0xa0, 0, 0, pkt, n * 2048);
			if (r < 0) {
				u8 rs[12] = { 0x03, 0, 0, 0, 18, 0 };
				ahci_cmd(p, 0xa0, 0, 0, rs, 18);
				r = ahci_cmd(p, 0xa0, 0, 0, pkt, n * 2048);
			}
		} else {
			r = ahci_cmd(p, 0x25, lba, n, NULL, n * 512);
		}
		if (r < 0) return -1;
		memcpy(out, p->bounce, n * bd->sector_size);
		out += n * bd->sector_size; lba += n; count -= n;
	}
	return 0;
}

#define P_SCTL 0x2c

static bool link_up(uintptr_t pb, u32 ms)
{
	u64 end = timer_us() + (u64)ms * 1000;
	do {
		if ((read32(pb + P_SSTS) & 0xf) == 3) return true;
		udelay(1000);
	} while (timer_us() < end);
	return false;
}

static void probe_port(struct pci_dev *pd, uintptr_t abar, int port)
{
	uintptr_t pb = abar + 0x100 + port * 0x80;

	/* Power on and spin up the device (needed when the HBA supports
	 * staggered spin-up, as Intel ICH parts do), then wait for the link. */
	write32(pb + P_CMD, read32(pb + P_CMD) | (1 << 1) | (1 << 2));
	if (!link_up(pb, 50)) {
		/* COMRESET */
		u32 sctl = read32(pb + P_SCTL) & ~0xfu;
		write32(pb + P_SCTL, sctl | 1);
		mdelay(2);
		write32(pb + P_SCTL, sctl);
		if (!link_up(pb, 1000)) {
			u32 det = read32(pb + P_SSTS) & 0xf;
			if (det) printf("SATA port %d: device present but no link (SStatus %x)\n", port, read32(pb + P_SSTS));
			return;
		}
	}
	if (port_stop(pb) < 0) {
		printf("SATA port %d: port would not stop\n", port);
		return;
	}

	struct ahci_port *p = zalloc(sizeof(*p));
	p->base = pb;
	p->clb = memalign(1024, 1024);
	p->fis = memalign(256, 256);
	p->tbl = memalign(128, 0x80 + NPRD * 16);
	p->bounce = memalign(4096, BOUNCE);
	if (!p->clb || !p->fis || !p->tbl || !p->bounce) return;
	memset(p->clb, 0, 1024);
	memset(p->fis, 0, 256);
	write32(pb + P_CLB, (uintptr_t)p->clb);
	write32(pb + P_CLB + 4, 0);
	write32(pb + P_FB, (uintptr_t)p->fis);
	write32(pb + P_FB + 4, 0);
	write32(pb + P_SERR, 0xffffffff);
	write32(pb + P_IS, 0xffffffff);
	write32(pb + P_CMD, read32(pb + P_CMD) | 0x10);	/* FRE */

	/* The signature arrives with the device's first register FIS; give a
	 * drive that is still spinning up time to send it. */
	u64 end = timer_us() + 10000000;
	while ((read32(pb + P_TFD) & 0x80) && timer_us() < end) mdelay(10);
	u32 sig = read32(pb + P_SIG);
	write32(pb + P_SERR, 0xffffffff);
	write32(pb + P_CMD, read32(pb + P_CMD) | 0x01);	/* ST */

	/* Unknown signature (some chipsets never latch it): ask the device. */
	u16 *id = (u16 *)p->bounce;
	bool ok;
	if (sig == 0xeb140101) {
		p->atapi = true;
		ok = ahci_cmd(p, 0xa1, 0, 0, NULL, 512) == 0;
	} else {
		ok = ahci_cmd(p, 0xec, 0, 0, NULL, 512) == 0;
		if (!ok && sig != 0x00000101) {
			p->atapi = true;
			ok = ahci_cmd(p, 0xa1, 0, 0, NULL, 512) == 0;
		}
	}
	if (!ok) {
		printf("SATA port %d: device did not answer IDENTIFY (signature %x)\n", port, sig);
		return;
	}

	struct blockdev *bd = bdev_register();
	if (!bd) return;
	bd->priv = p;
	bd->driver = "ahci";
	bd->read = ahci_read;
	for (int i = 0; i < 20; i++) {
		bd->model[i * 2] = id[27 + i] >> 8;
		bd->model[i * 2 + 1] = id[27 + i] & 0xff;
	}
	for (int i = 39; i >= 0 && (bd->model[i] == ' ' || !bd->model[i]); i--) bd->model[i] = 0;
	if (p->atapi) {
		bd->type = BDEV_CDROM;
		bd->sector_size = 2048;
		bd->nsectors = 0xffffffff;
		u8 pkt[12] = { 0x25 };
		for (int t = 0; t < 3; t++) {
			if (ahci_cmd(p, 0xa0, 0, 0, pkt, 8) == 0) {
				u8 *c = p->bounce;
				bd->nsectors = (((u32)c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]) + 1;
				break;
			}
		}
		snprintf(bd->path, sizeof(bd->path), "%s/cdrom@%d,0", pd->path, port);
	} else {
		bd->type = BDEV_DISK;
		bd->sector_size = 512;
		u64 n48;
		memcpy(&n48, &id[100], 8);
		bd->nsectors = (id[83] & (1 << 10)) && n48 ? n48 : (id[60] | ((u32)id[61] << 16));
		snprintf(bd->path, sizeof(bd->path), "%s/disk@%d,0", pd->path, port);
	}
}

void ahci_probe(void)
{
	for (int i = 0; i < pci_ndevs; i++) {
		struct pci_dev *pd = &pci_devs[i];
		/* Match on the SATA subclass only: coreboot's ICH7 code writes 0x8f
		 * to the programming interface byte even in AHCI mode. */
		if (pd->class != 1 || pd->subclass != 6) continue;
		uintptr_t abar = pci_bar_mem(pd, 5);
		if (!abar) continue;
		pd->claimed = true;
		pci_enable(pd);
		write32(abar + HBA_GHC, read32(abar + HBA_GHC) | 0x80000000u);
		u32 pi = read32(abar + HBA_PI);
		if (!pi) pi = (2u << (read32(abar + HBA_CAP) & 0x1f)) - 1;	/* PI not set up */
		for (int port = 0; port < 32; port++)
			if (pi & (1u << port)) probe_port(pd, abar, port);
	}
}
