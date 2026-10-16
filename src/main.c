/*
 * SunBiosPort: OpenBoot for x86, as a coreboot (or Multiboot) payload.
 *
 * Power-on sequence modelled on a SPARCstation 5 running OBP 2.15:
 * memory and device probing messages (when diag-switch? is set), the NVRAM
 * checksum message, the banner with the Sun logo, auto-boot, and finally
 * the "ok" prompt.
 */
#include "lib.h"
#include "console.h"
#include "platform.h"
#include "pci.h"
#include "blockdev.h"
#include "nvram.h"
#include "forth.h"
#include "boot.h"
#include "obp.h"

#ifndef HEAP_SIZE
#define HEAP_SIZE (48u * 1024 * 1024)
#endif

static void setup_heap(void)
{
	uintptr_t start = ALIGN_UP((uintptr_t)__image_end, 4096);
	uintptr_t end = start + HEAP_SIZE;
	/* shrink to the RAM that is actually there */
	while (end > start + 4 * 1024 * 1024 && !mem_is_ram(start, end - start)) end -= 1024 * 1024;
	heap_init(start, end);
}

static void setup_video(void)
{
	struct fb_info fb;
	if (sysinfo.have_fb && sysinfo.fb.bpp >= 15) {
		fb_init(&sysinfo.fb);
		if (fb_active()) return;
	}
	if (video_init_bochs(&fb)) {
		fb_init(&fb);
		if (fb_active()) return;
	}
	/* Legacy VGA text mode, if something set it up (no logo possible). */
	if (sysinfo.bios_present || (inb(0x3cc) != 0xff)) {
		memset(&fb, 0, sizeof(fb));
		fb.text_mode = 1;
		fb_init(&fb);
	}
}

static void cpu_names(char *vendor, char *brand)
{
	u32 a, b, c, d;
	cpuid(0, 0, &a, &b, &c, &d);
	memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4);
	vendor[12] = 0;
	brand[0] = 0;
	cpuid(0x80000000, 0, &a, &b, &c, &d);
	if (a >= 0x80000004) {
		for (u32 i = 0; i < 3; i++) {
			u32 *r = (u32 *)(brand + i * 16);
			cpuid(0x80000002 + i, 0, &r[0], &r[1], &r[2], &r[3]);
		}
		brand[48] = 0;
		char *s = brand;
		while (*s == ' ') s++;
		memmove(brand, s, strlen(s) + 1);
	}
}

static void probe_memory_banks(void)
{
	if (sysinfo.ndimms) {
		for (int i = 0; i < sysinfo.ndimms; i++) {
			if (sysinfo.dimm_mb[i])
				printf("Probing Memory Bank #%d %u Megabytes of DRAM\n", i, sysinfo.dimm_mb[i]);
			else
				printf("Probing Memory Bank #%d Nothing there\n", i);
		}
		return;
	}
	int bank = 0;
	for (int i = 0; i < sysinfo.nmem; i++) {
		struct memrange *m = &sysinfo.mem[i];
		if (m->type != MEM_RAM || m->size < (1 << 20)) continue;
		printf("Probing Memory Bank #%d %llu Megabytes of DRAM\n", bank++, m->size >> 20);
	}
}

static void probe_devices(void)
{
	for (int i = 0; i < pci_ndevs; i++) {
		struct pci_dev *p = &pci_devs[i];
		char parent[64];
		strlcpy(parent, p->path, sizeof(parent));
		*strrchr(parent, '/') = 0;
		char line[256];
		int n = snprintf(line, sizeof(line), "Probing %s at %x,%x  %s", parent, p->dev, p->fn, p->name);
		/* children found below this function: disks, cdroms */
		const char *seen[8];
		int nseen = 0;
		size_t pl = strlen(p->path);
		for (int j = 0; j < nbdevs; j++) {
			const char *bp = bdevs[j]->path;
			if (strncmp(bp, p->path, pl) || bp[pl] != '/') continue;
			const char *leaf = bdevs[j]->type == BDEV_CDROM ? "cdrom" : "disk";
			bool dup = false;
			for (int k = 0; k < nseen; k++) if (seen[k] == leaf) dup = true;
			if (dup || nseen >= 8) continue;
			seen[nseen++] = leaf;
			n += snprintf(line + n, sizeof(line) - n, " %s", leaf);
		}
		printf("%s\n", line);
	}
}

/* Stop-A (F1-A on a PC keyboard, as in Solaris x86; BREAK on ttya) at any
 * point during power-on stops auto-boot. Other keys are ignored. */
static bool stop_a;

static void poll_stop_a(void)
{
	int c;
	while ((c = console_getc_nb()) >= 0)
		if (c == KEY_STOPA) stop_a = true;
}

static bool autoboot_aborted(void)
{
	/* a short, invisible window so a Stop-A already being held is seen */
	u64 end = timer_us() + 300000;
	while (timer_us() < end && !stop_a) poll_stop_a();
	return stop_a;
}

void c_main(u32 magic, u32 mbi, u32 arg)
{
	(void)magic; (void)mbi; (void)arg;
	platform_init();
	setup_heap();
	timer_init();
	if (sysinfo.have_serial && (sysinfo.serial_port || sysinfo.serial_mmio))
		serial_init(sysinfo.serial_port, sysinfo.serial_baud, sysinfo.serial_mmio, sysinfo.serial_regw);
	else
		serial_init(0x3f8, 115200, 0, 1);
	pci_scan();
	setup_video();
	kbd_init();
	smbios_init();
	net_probe();
	nv_init();
	if (!strcmp(nv_get("output-device"), "ttya")) console_set_output(false, true);

	bool reset = nv_was_reset();
	bool diag = reset || nv_get_bool("diag-switch?");
	if (diag) probe_memory_banks();
	poll_stop_a();
	if (reset) {
		printf("Incorrect configuration checksum; \n");
		printf("Setting NVRAM parameters to default values.\n");
		printf("Setting diag-switch? NVRAM parameter to true\n");
		nv_set("diag-switch?", "true");
	}
	char vendor[13], brand[49];
	cpu_names(vendor, brand);
	if (diag) printf("Probing CPU %s,%s\n", vendor, brand[0] ? brand : "x86");

	bdev_probe_all();
	poll_stop_a();
	if (diag) probe_devices();

	devalias_init();
	devtree_init();
	forth_init();
	obp_banner();
	poll_stop_a();

	if (nv_get_bool("use-nvramrc?")) forth_evaluate(nv_get("nvramrc"));

	if (nv_get_bool("auto-boot?") && !autoboot_aborted()) {
		boot_command("");
		printf("\n");
	}
	printf("Type  help  for more information\n");
	forth_quit();
}
