/* PCI configuration access and bus enumeration (mechanism #1). coreboot
 * has already assigned resources; we only discover and name devices. */
#include "pci.h"

struct pci_dev pci_devs[MAX_PCI];
int pci_ndevs;

static u32 addr(u8 b, u8 d, u8 f, u8 o) { return 0x80000000u | (b << 16) | (d << 11) | (f << 8) | (o & 0xfc); }

u32 pci_read32(u8 b, u8 d, u8 f, u8 o) { outl(0xcf8, addr(b, d, f, o)); return inl(0xcfc); }
u16 pci_read16(u8 b, u8 d, u8 f, u8 o) { outl(0xcf8, addr(b, d, f, o)); return inw(0xcfc + (o & 2)); }
u8 pci_read8(u8 b, u8 d, u8 f, u8 o) { outl(0xcf8, addr(b, d, f, o)); return inb(0xcfc + (o & 3)); }
void pci_write32(u8 b, u8 d, u8 f, u8 o, u32 v) { outl(0xcf8, addr(b, d, f, o)); outl(0xcfc, v); }
void pci_write16(u8 b, u8 d, u8 f, u8 o, u16 v) { outl(0xcf8, addr(b, d, f, o)); outw(0xcfc + (o & 2), v); }
void pci_write8(u8 b, u8 d, u8 f, u8 o, u8 v) { outl(0xcf8, addr(b, d, f, o)); outb(0xcfc + (o & 3), v); }

const char *pci_class_name(u8 c, u8 s, u8 p)
{
	switch (c) {
	case 0x01:
		switch (s) {
		case 0x00: return "scsi";
		case 0x01: return "ide";
		case 0x04: return "raid";
		case 0x05: return "ata";
		case 0x06: return p == 1 ? "sata" : "sata";
		case 0x07: return "sas";
		case 0x08: return "nvme";
		}
		return "storage";
	case 0x02: return s == 0 ? "ethernet" : "network";
	case 0x03: return "display";
	case 0x04: return s == 1 ? "sound" : s == 3 ? "sound" : "multimedia";
	case 0x05: return "memory-controller";
	case 0x06:
		switch (s) {
		case 0x00: return "host";
		case 0x01: return "isa";
		case 0x04: return "pci";
		case 0x80: return "bridge";
		}
		return "bridge";
	case 0x07: return s == 0 ? "serial" : "communication";
	case 0x08: return "system-peripheral";
	case 0x09: return "input";
	case 0x0c:
		switch (s) {
		case 0x03: return p == 0x30 ? "xhci" : p == 0x20 ? "ehci" : p == 0x10 ? "ohci" : "usb";
		case 0x05: return "smbus";
		case 0x00: return "firewire";
		}
		return "serial-bus";
	case 0x0d: return "wireless";
	case 0x11: return "signal-processing";
	}
	return "unknown";
}

static void probe_fn(u8 b, u8 d, u8 f)
{
	u32 id = pci_read32(b, d, f, 0);
	if ((id & 0xffff) == 0xffff || pci_ndevs >= MAX_PCI) return;
	struct pci_dev *p = &pci_devs[pci_ndevs++];
	memset(p, 0, sizeof(*p));
	p->bus = b; p->dev = d; p->fn = f;
	p->vendor = id & 0xffff;
	p->device = id >> 16;
	u32 cr = pci_read32(b, d, f, 8);
	p->rev = cr & 0xff;
	p->progif = (cr >> 8) & 0xff;
	p->subclass = (cr >> 16) & 0xff;
	p->class = cr >> 24;
	p->hdr = pci_read8(b, d, f, 0x0e);
	int nbars = (p->hdr & 0x7f) == 0 ? 6 : (p->hdr & 0x7f) == 1 ? 2 : 0;
	for (int i = 0; i < nbars; i++) p->bar[i] = pci_read32(b, d, f, 0x10 + i * 4);
	strlcpy(p->name, pci_class_name(p->class, p->subclass, p->progif), sizeof(p->name));
	char parent[24] = "/pci@0,0";
	if (b) snprintf(parent, sizeof(parent), "/pci@0,0/pci@b%x", b);
	if (f) snprintf(p->path, sizeof(p->path), "%s/%s@%x,%x", parent, p->name, d, f);
	else snprintf(p->path, sizeof(p->path), "%s/%s@%x", parent, p->name, d);
}

void pci_scan(void)
{
	pci_ndevs = 0;
	for (int b = 0; b < 256; b++) {
		for (int d = 0; d < 32; d++) {
			u32 id = pci_read32(b, d, 0, 0);
			if ((id & 0xffff) == 0xffff) continue;
			int mf = pci_read8(b, d, 0, 0x0e) & 0x80;
			for (int f = 0; f < (mf ? 8 : 1); f++) probe_fn(b, d, f);
		}
		/* Stop early when no bridges lead beyond this bus. */
		if (b > 0) {
			bool more = false;
			for (int i = 0; i < pci_ndevs; i++) {
				struct pci_dev *p = &pci_devs[i];
				if ((p->hdr & 0x7f) == 1) {
					u8 sub = pci_read8(p->bus, p->dev, p->fn, 0x1a);
					if (sub > b) more = true;
				}
			}
			if (!more) break;
		}
	}
}

uintptr_t pci_bar_mem(struct pci_dev *d, int i)
{
	u32 v = d->bar[i];
	if (v & 1) return 0;
	if (((v >> 1) & 3) == 2 && i < 5 && d->bar[i + 1]) return 0;	/* above 4G */
	return v & ~0xfu;
}

u16 pci_bar_io(struct pci_dev *d, int i)
{
	u32 v = d->bar[i];
	return (v & 1) ? (v & ~3u) : 0;
}

void pci_enable(struct pci_dev *d)
{
	u16 cmd = pci_read16(d->bus, d->dev, d->fn, 4);
	pci_write16(d->bus, d->dev, d->fn, 4, cmd | 0x7);
}
