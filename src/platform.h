#ifndef PLATFORM_H
#define PLATFORM_H

#include "lib.h"
#include "console.h"

#define MEM_RAM      1
#define MEM_RESERVED 2
#define MEM_ACPI     3
#define MEM_NVS      4
#define MEM_UNUSABLE 5
#define MEM_CBTABLE  16

struct memrange { u64 base, size; u32 type; };

struct sysinfo {
	bool from_coreboot, from_multiboot, bios_present;
	struct memrange mem[64];
	int nmem;
	bool have_fb;
	struct fb_info fb;
	bool have_serial;
	u16 serial_port;
	uintptr_t serial_mmio;
	u32 serial_baud, serial_regw;
	bool have_bootmedia;
	u64 cbfs_offset, cbfs_size, boot_media_size;
	uintptr_t cb_header;
	char cb_vendor[64], cb_part[64], cb_version[64];
	char mb_cmdline[256];
	char mb_loader[64];
	uintptr_t acpi_rsdp;
	/* SMBIOS */
	char sys_vendor[64], sys_product[64], sys_serial[64], bios_version[64];
	u8 sys_uuid[16];
	int ndimms;
	u32 dimm_mb[16];
	char dimm_loc[16][24];
	/* derived */
	u64 total_ram;
	u8 mac[6];
	bool have_mac;
	char mac_dev[96];
};

extern struct sysinfo sysinfo;
extern u32 boot_magic, boot_mbi, boot_arg;

void platform_init(void);
void smbios_init(void);
u64 mem_top_below(u64 limit);
bool mem_is_ram(u64 base, u64 size);
void reset_system(void);
void power_off(void);

/* Physical extent of the firmware image, from the linker script. */
extern char __image_start[], __image_end[];

#endif
