#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include "lib.h"

enum { BDEV_DISK, BDEV_CDROM };

struct blockdev {
	char alias[16];		/* disk0, cdrom0, ... */
	char path[96];		/* /pci@0,0/ide@1,1/disk@0,0 */
	char model[48];
	int type;
	u32 sector_size;
	u64 nsectors;
	int (*read)(struct blockdev *bd, u64 lba, u32 count, void *buf);
	void *priv;
	int unit;		/* controller-specific */
	const char *driver;
};

#define MAX_BDEV 16
extern struct blockdev *bdevs[MAX_BDEV];
extern int nbdevs;

struct blockdev *bdev_register(void);
void bdev_probe_all(void);
void ata_probe(void);
void ahci_probe(void);
void nvme_probe(void);
void ahci_dump(void);
struct blockdev *bdev_find(const char *name);
int bdev_read(struct blockdev *bd, u64 lba, u32 count, void *buf);

/* A volume is a byte-addressable window (whole disk or a partition). */
struct volume {
	struct blockdev *bd;
	u64 start;		/* byte offset on the device */
	u64 size;		/* bytes */
	char desc[48];
};

int vol_read(struct volume *v, u64 off, u32 len, void *buf);

#endif
