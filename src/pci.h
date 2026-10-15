#ifndef PCI_H
#define PCI_H

#include "lib.h"

struct pci_dev {
	u8 bus, dev, fn;
	u16 vendor, device;
	u8 class, subclass, progif, rev;
	u8 hdr;
	u32 bar[6];
	char name[24];		/* Open Firmware generic name */
	char path[64];		/* device tree path */
};

#define MAX_PCI 128
extern struct pci_dev pci_devs[MAX_PCI];
extern int pci_ndevs;

u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off);
u16 pci_read16(u8 bus, u8 dev, u8 fn, u8 off);
u8 pci_read8(u8 bus, u8 dev, u8 fn, u8 off);
void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v);
void pci_write16(u8 bus, u8 dev, u8 fn, u8 off, u16 v);
void pci_write8(u8 bus, u8 dev, u8 fn, u8 off, u8 v);

void pci_scan(void);
uintptr_t pci_bar_mem(struct pci_dev *d, int bar);
u16 pci_bar_io(struct pci_dev *d, int bar);
void pci_enable(struct pci_dev *d);
const char *pci_class_name(u8 cls, u8 sub, u8 progif);

#endif
