#ifndef BOOT_H
#define BOOT_H

#include "lib.h"
#include "fs.h"
#include "console.h"

/* A readable boot image: a file on a filesystem or a buffer in memory. */
struct src {
	u64 size;
	struct file *file;
	const u8 *mem;
	char name[256];
};

int src_read(struct src *s, u64 off, u32 len, void *buf);

#define MAX_MODS 16

struct boot_entry {
	char title[96];
	char kernel[256];
	char kargs[768];
	int nmods;
	char mods[MAX_MODS][256];
	char modargs[MAX_MODS][256];
	int ninitrd;
	char initrd[4][256];
	bool chain;	/* chainloader +1 / legacy */
};

struct boot_config {
	int nentries, def;
	struct boot_entry *e;
	char path[64];
};

/* Physical memory reservations used while placing an OS. */
void phys_reset(void);
void phys_reserve(u64 start, u64 end);
u64 phys_alloc(u64 size, u64 align, u64 min, u64 max, bool high);
bool phys_free_range(u64 start, u64 end);

/* Progress messages OpenBoot would not print; shown when diag-switch? is set. */
bool boot_verbose(void);
#define dprintf(...) do { if (boot_verbose()) printf(__VA_ARGS__); } while (0)

typedef int (*src_opener)(void *ctx, const char *path, struct src *s);

int boot_command(const char *args);
int boot_config_text(char *text, int kind, const char *dir, const char *args, src_opener open, void *ctx);
int boot_entries_command(const char *dev);
int boot_load_image(struct src *k, const char *cmdline, struct src *mods, const char **modcmd,
		    int nmods, struct src *initrd, int ninitrd);
int load_command(const char *args, uintptr_t addr, u32 *size);
int go_command(uintptr_t addr, u32 size);
int dir_command(const char *args);
int cat_command(const char *args);
int legacy_boot(int bios_drive);
int cbfs_boot(const char *name);
void cbfs_list(void);

/* image formats */
int multiboot_boot(struct src *k, u8 *hdr_buf, u32 hdr_off, const char *cmdline,
		   struct src *mods, const char **modcmd, int nmods);
int linux_boot(struct src *k, const char *cmdline, struct src *initrd, int ninitrd);
int elf_boot(struct src *k, const char *cmdline);

/* video hand-off (vga.c) */
bool video_init_bochs(struct fb_info *fb);
void vga_save_text(void);
bool vga_restore_text(void);
bool vga_text_saved(void);

/* net.c */
int net_boot(const char *file, const char *args);
int net_probe(void);

/* lzma.c / lz4 */
int lzma_decompress(const u8 *src, u32 srclen, u8 *dst, u32 dstlen, u32 *outlen);
int lz4_decompress(const u8 *src, u32 srclen, u8 *dst, u32 dstlen, u32 *outlen);

void call_payload(u32 entry, u32 arg) NORETURN;
void boot_jump(u32 entry, u32 eax, u32 ebx, u32 esi) NORETURN;

#endif
