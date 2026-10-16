/* OpenBoot user interface commands, built on the Forth interpreter. */
#include "forth.h"
#include "console.h"
#include "platform.h"
#include "nvram.h"
#include "boot.h"
#include "pci.h"
#include "blockdev.h"
#include "obp.h"

#define CMD(n) static void n(struct word *w __attribute__((unused)))

static const char *keyboard_type(void) { return kbd_present() ? "Keyboard Present" : "No Keyboard"; }

void obp_cpu_model(char *out, size_t n)
{
	const char *nv = nv_get("oem-banner?");
	(void)nv;
	if (sysinfo.sys_product[0] && strcmp(sysinfo.sys_product, "Not Specified"))
		snprintf(out, n, "%s", sysinfo.sys_product);
	else if (sysinfo.cb_part[0])
		snprintf(out, n, "%s %s", sysinfo.cb_vendor, sysinfo.cb_part);
	else
		snprintf(out, n, "PC-compatible x86 system");
}

u32 obp_serial(void)
{
	if (sysinfo.have_mac) return (sysinfo.mac[3] << 16) | (sysinfo.mac[4] << 8) | sysinfo.mac[5];
	u32 h = 0;
	for (int i = 0; i < 16; i++) h = h * 31 + sysinfo.sys_uuid[i];
	return h & 0xffffff;
}

u32 obp_hostid(void) { return 0x80000000u | obp_serial(); }

static void fmt_enet(char *b, size_t n)
{
	const u8 *m = sysinfo.mac;
	snprintf(b, n, "%x:%x:%x:%x:%x:%x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void obp_banner(void)
{
	bool logo = fb_is_graphics();
	int indent = logo ? 128 / 12 + 2 : 0;
	console_putc('\n');
	/* keep the six banner lines plus the logo on screen */
	if (logo && fb_rows() - fb_line() < 7) fb_set_line(fb_rows() - 7);
	int start = fb_line();
	char model[96];
	obp_cpu_model(model, sizeof(model));
	if (nv_get_bool("oem-banner?")) {
		printf("\n%*s%s\n\n", indent, "", nv_get("oem-banner"));
	} else {
		/* one line, like "SPARCstation 5, Keyboard Present" */
		int room = (fb_active() ? fb_cols() : 80) - indent - 2 - (int)strlen(keyboard_type()) - 1;
		if ((int)strlen(model) > room) {
			char *paren = strstr(model, " (");
			if (paren) *paren = 0;
		}
		if ((int)strlen(model) > room && room > 0) model[room] = 0;
		printf("%*s%s, %s\n", indent, "", model, keyboard_type());
		u32 mb = (u32)(sysinfo.total_ram >> 20);
		/* report installed memory, rounding up the firmware-reserved holes */
		u32 inst = 0;
		for (int i = 0; i < sysinfo.ndimms; i++) inst += sysinfo.dimm_mb[i];
		if (inst >= mb) mb = inst;
		else mb = ALIGN_UP(mb, 16);
		printf("%*sROM Rev. %s, %u MB memory installed, Serial #%u.\n", indent, "", OBP_REV, mb, obp_serial());
		if (sysinfo.have_mac) {
			char e[32];
			fmt_enet(e, sizeof(e));
			printf("%*sEthernet address %s, Host ID: %08x.\n", indent, "", e, obp_hostid());
		} else {
			printf("%*sNo Ethernet, Host ID: %08x.\n", indent, "", obp_hostid());
		}
	}
	printf("\n\n");
	if (logo) {
		/* align the 64-pixel logo with the first banner line, as OBP does */
		fb_draw_logo(start);
	}
	printf("\n");
}

CMD(c_banner) { obp_banner(); }
CMD(c_boot) { boot_command(forth_parse_rest()); printf("\n"); }
CMD(c_reset) { reset_system(); }
CMD(c_poweroff) { power_off(); }
CMD(c_printenv) { nv_print(forth_parse_rest()); }
CMD(c_setenv)
{
	char *n = forth_parse_word();
	if (!n) { printf("Usage: setenv name value\n"); return; }
	char name[64];
	strlcpy(name, n, sizeof(name));
	char *v = forth_parse_rest();
	int r = nv_set(name, v);
	if (r == -1) printf("Unknown option: %s\n", name);
	else if (r == -2) printf("%s must be true or false\n", name);
	else printf("%s = %s\n", name, nv_get(name));
}
CMD(c_setdefault)
{
	char *n = forth_parse_word();
	if (!n || nv_set_default(n) < 0) printf("Unknown option\n");
}
CMD(c_setdefaults) { printf("Setting NVRAM parameters to default values.\n"); nv_set_defaults(); }
CMD(c_devalias)
{
	char *n = forth_parse_word();
	if (!n) { devalias_print(NULL); return; }
	char name[64];
	strlcpy(name, n, sizeof(name));
	char *p = forth_parse_rest();
	if (*p) devalias_set(name, p);
	else devalias_print(name);
}
CMD(c_showdevs) { show_devs(forth_parse_rest()); }
CMD(c_showdisks)
{
	for (int i = 0; i < nbdevs; i++) {
		struct blockdev *bd = bdevs[i];
		printf("%-8s %s\n", bd->alias, bd->path);
	}
}

static void print_size(u64 bytes)
{
	if (bytes >= (1ull << 30)) printf("%llu GB", bytes >> 30);
	else printf("%llu MB", bytes >> 20);
}

CMD(c_probedisks)
{
	for (int i = 0; i < nbdevs; i++) {
		struct blockdev *bd = bdevs[i];
		printf("%-6s %-5s %-40s ", bd->alias, bd->driver, bd->model);
		if (bd->type == BDEV_CDROM) printf("Removable Read Only device");
		else print_size(bd->nsectors * bd->sector_size);
		printf("\n       %s\n", bd->path);
	}
	if (!nbdevs) printf("No disks or CD-ROM drives found\n");
}

CMD(c_probesata) { c_probedisks(w); printf("\n"); ahci_dump(); }

CMD(c_probepci)
{
	for (int i = 0; i < pci_ndevs; i++) {
		struct pci_dev *p = &pci_devs[i];
		printf("%02x:%02x.%x  %04x,%04x  class %02x%02x%02x  %s\n", p->bus, p->dev, p->fn, p->vendor,
		       p->device, p->class, p->subclass, p->progif, p->path);
	}
}

CMD(c_version)
{
	printf("Release %s, OpenBoot %s for x86 (SunBiosPort), built %s\n", OBP_REV, OBP_REV, __DATE__);
	if (sysinfo.from_coreboot) printf("Running as a coreboot payload; coreboot %s\n", sysinfo.cb_version);
	else if (sysinfo.from_multiboot) printf("Started by a Multiboot loader%s%s\n", sysinfo.mb_loader[0] ? ": " : "", sysinfo.mb_loader);
	if (sysinfo.bios_version[0]) printf("Platform firmware: %s\n", sysinfo.bios_version);
}

CMD(c_speed)
{
	u32 a, b, c, d;
	char brand[49] = "";
	cpuid(0x80000000, 0, &a, &b, &c, &d);
	if (a >= 0x80000004) {
		for (u32 i = 0; i < 3; i++) {
			u32 *r = (u32 *)(brand + i * 16);
			cpuid(0x80000002 + i, 0, &r[0], &r[1], &r[2], &r[3]);
		}
	}
	printf("CPU  Speed : %u MHz  %s\n", cpu_mhz(), brand);
}

CMD(c_enet)
{
	char e[32];
	if (!sysinfo.have_mac) { printf("No Ethernet interface\n"); return; }
	fmt_enet(e, sizeof(e));
	printf("%s\n", e);
}

CMD(c_idprom)
{
	u8 id[32];
	memset(id, 0, sizeof(id));
	id[0] = 1;			/* format */
	id[1] = 0x80;			/* machine type */
	memcpy(id + 2, sysinfo.mac, 6);
	u32 s = obp_serial();
	id[12] = s >> 16; id[13] = s >> 8; id[14] = s;
	u8 sum = 0;
	for (int i = 0; i < 15; i++) sum ^= id[i];
	id[15] = sum;
	printf("Format/Type: %x %x Ethernet: %x %x %x %x %x %x Serial: %x %x %x Chk: %x\n", id[0], id[1],
	       id[2], id[3], id[4], id[5], id[6], id[7], id[12], id[13], id[14], id[15]);
}

CMD(c_memory)
{
	static const char *types[] = { "", "available", "reserved", "ACPI", "ACPI NVS", "unusable" };
	for (int i = 0; i < sysinfo.nmem; i++) {
		struct memrange *m = &sysinfo.mem[i];
		const char *t = m->type < 6 ? types[m->type] : m->type == 16 ? "coreboot tables" : "other";
		printf("%016llx-%016llx  %s\n", m->base, m->base + m->size - 1, t);
	}
	printf("Total available: %llu MB\n", sysinfo.total_ram >> 20);
}

CMD(c_moduleinfo)
{
	u32 a, b, c, d;
	char vendor[13];
	cpuid(0, 0, &a, &b, &c, &d);
	memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4); vendor[12] = 0;
	cpuid(1, 0, &a, &b, &c, &d);
	printf("CPU: %s family %x model %x stepping %x, %u MHz\n", vendor, (a >> 8) & 0xf, (a >> 4) & 0xf, a & 0xf, cpu_mhz());
	for (int i = 0; i < sysinfo.ndimms; i++)
		printf("Memory module %s: %u MB\n", sysinfo.dimm_loc[i], sysinfo.dimm_mb[i]);
}

static u8 rtc(u8 r)
{
	outb(0x70, r);
	u8 v = inb(0x71);
	outb(0x70, 0x0b);
	if (!(inb(0x71) & 4)) v = (v & 15) + (v >> 4) * 10;
	return v;
}

CMD(c_watchclock)
{
	printf("Watching the 'seconds' register of the real time clock chip.\n");
	printf("It should be 'ticking' once a second.\nType any key to stop.\n");
	u8 last = 0xff;
	while (console_getc_nb() < 0) {
		u8 s = rtc(0);
		if (s != last) { printf("\r%2d", s); last = s; }
		mdelay(20);
	}
	printf("\n");
}

CMD(c_timedate)
{
	printf("%02d/%02d/20%02d %02d:%02d:%02d\n", rtc(8), rtc(7), rtc(9), rtc(4), rtc(2), rtc(0));
}

static void memtest(u32 mb)
{
	uintptr_t hs, he;
	heap_bounds(&hs, &he);
	u32 len = MIN(mb << 20, (u32)(he - hs) / 2);
	u32 *buf = malloc(len);
	if (!buf) { printf("No memory to test\n"); return; }
	u32 n = len / 4, bad = 0;
	static const u32 pat[] = { 0x00000000, 0xffffffff, 0xaaaaaaaa, 0x55555555 };
	for (u32 p = 0; p < 4; p++) {
		for (u32 i = 0; i < n; i++) buf[i] = pat[p] ^ i;
		for (u32 i = 0; i < n; i++) if (buf[i] != (pat[p] ^ i)) bad++;
	}
	free(buf);
	printf("Testing %u megabytes of memory ... %s\n", len >> 20, bad ? "FAILED" : "Completed.");
}

CMD(c_test)
{
	char *d = forth_parse_rest();
	if (!strcmp(d, "/memory") || !strcmp(d, "memory")) { memtest(nv_get_int("selftest-#megs")); return; }
	if (!strcmp(d, "net")) {
		if (!sysinfo.have_mac) { printf("No network device\n"); return; }
		printf("Using %s\n", sysinfo.mac_dev);
		printf("Ethernet address present, interface found.\n");
		return;
	}
	struct blockdev *bd = bdev_find(d);
	if (!bd && devalias_get(d)) bd = bdev_find(devalias_get(d));
	if (bd) {
		u8 *b = malloc(bd->sector_size);
		int r = b ? bdev_read(bd, 0, 1, b) : -1;
		free(b);
		printf("%s: %s\n", bd->path, r == 0 ? "read test passed" : "read test FAILED");
		return;
	}
	printf("Usage: test <memory | net | disk | cdrom | devalias>\n");
}

CMD(c_testall)
{
	memtest(1);
	for (int i = 0; i < nbdevs; i++) {
		u8 *b = malloc(bdevs[i]->sector_size);
		int r = b ? bdev_read(bdevs[i], 0, 1, b) : -1;
		free(b);
		printf("Testing %s ... %s\n", bdevs[i]->path, r == 0 ? "passed" : "FAILED");
	}
}

static u32 load_size;
CMD(c_load)
{
	char *a = forth_parse_rest();
	char spec[300];
	if (!*a) snprintf(spec, sizeof(spec), "%s %s", nv_get("boot-device"), nv_get("boot-file"));
	else strlcpy(spec, a, sizeof(spec));
	uintptr_t base = nv_get_int("load-base");
	if (load_command(spec, base, &load_size) == 0) printf("Loaded %u bytes at %lx\n", load_size, base);
}
CMD(c_go)
{
	uintptr_t base = nv_get_int("load-base");
	go_command(base, load_size);
}
CMD(c_dir) { dir_command(forth_parse_rest()); }
CMD(c_cat) { cat_command(forth_parse_rest()); }
CMD(c_entries) { boot_entries_command(forth_parse_rest()); }
CMD(c_cbfslist) { cbfs_list(); }
CMD(c_cbfsboot) { char *n = forth_parse_rest(); cbfs_boot(*n ? n : "img/seabios"); }
CMD(c_dev) { devtree_dev(forth_parse_rest()); }
CMD(c_ls) { devtree_ls(); }
CMD(c_pwd) { devtree_pwd(); }
CMD(c_props) { devtree_props(); }
CMD(c_nvramrc) { forth_evaluate(nv_get("nvramrc")); }
CMD(c_cfg)
{
	cell reg = fpop();
	fpush(pci_read32((reg >> 16) & 0xff, (reg >> 11) & 0x1f, (reg >> 8) & 7, reg & 0xfc));
}
CMD(c_cfgw)
{
	cell reg = fpop(), v = fpop();
	pci_write32((reg >> 16) & 0xff, (reg >> 11) & 0x1f, (reg >> 8) & 7, reg & 0xfc, v);
}
CMD(c_cfgb)
{
	cell reg = fpop();
	fpush(pci_read8((reg >> 16) & 0xff, (reg >> 11) & 0x1f, (reg >> 8) & 7, reg & 0xff));
}
CMD(c_cfgww)
{
	cell reg = fpop();
	fpush(pci_read16((reg >> 16) & 0xff, (reg >> 11) & 0x1f, (reg >> 8) & 7, reg & 0xfe));
}
CMD(c_screenclear) { console_putc('\f'); console_flush(); }

CMD(c_help)
{
	char *t = forth_parse_rest();
	if (!*t) {
		printf("Enter 'help command-name' or 'help category-name' for more help\n");
		printf("(Use ONLY the first word of a category description)\n");
		printf("Examples:  help boot    help nvram    help dump\n");
		printf("Main categories are:\n");
		printf("File download and boot\nResets\nSystem and boot configuration parameters\n");
		printf("Diag (diagnostic routines)\nDevice tree browsing\nMemory access\n");
		printf("Arithmetic\nNumber output\nLine editor\nForth words\n");
		return;
	}
	if (!strncmp(t, "file", 4) || !strcmp(t, "boot")) {
		printf("boot [<device-specifier>] [<filename>] [<options>]\n");
		printf("    examples:\n");
		printf("       boot                  boot from boot-device / boot-file\n");
		printf("       boot cdrom            boot x86 Solaris / Linux install media (GRUB menu)\n");
		printf("       boot disk1:b          boot from the 2nd partition of disk1\n");
		printf("       boot disk -s          append -s (single user) to the kernel arguments\n");
		printf("       boot disk /boot/vmlinuz root=/dev/sda1   boot a named kernel\n");
		printf("       boot net              DHCP/BOOTP then TFTP (pxegrub menu.lst supported)\n");
		printf("       boot legacy           hand over to a legacy PC BIOS (SeaBIOS from CBFS)\n");
		printf(".entries <dev>    list the boot menu found on a device; select with\n");
		printf("                  setenv boot-entry <n>  (-1 = the menu's default)\n");
		printf("load <dev> <file> load a file at load-base;  go  execute it\n");
		printf("dir <dev> [path]  list a directory;  cat <dev> <path>  show a text file\n");
		printf("cbfs-list, cbfs-boot <name>   coreboot CBFS payloads\n");
		return;
	}
	if (!strncmp(t, "reset", 5)) {
		printf("reset-all         reset the machine\npower-off         power the machine off (ACPI S5)\n");
		return;
	}
	if (!strncmp(t, "system", 6) || !strncmp(t, "nvram", 5)) {
		printf("printenv [name]   show configuration parameters\n");
		printf("setenv name value set a configuration parameter\n");
		printf("set-default name  reset a parameter;  set-defaults  reset all\n");
		printf("devalias [name [path]]   show or create device aliases\n");
		return;
	}
	if (!strncmp(t, "diag", 4)) {
		printf("test <device>     test memory, net, disk ...;  test-all\n");
		printf("probe-pci  probe-ide  probe-scsi  probe-sata  probe-nvme\n");
		printf("watch-clock  .version  .speed  .enet-addr  .idprom  .memory  module-info  banner\n");
		return;
	}
	if (!strncmp(t, "device", 6)) {
		printf("show-devs [path]  show-disks  dev <path>  ls  pwd  .properties\n");
		return;
	}
	if (!strncmp(t, "memory", 6) || !strcmp(t, "dump")) {
		printf("dump ( addr len -- )  @ ! c@ c! w@ w! l@ l!  fill move comp\n");
		printf("inb outb inw outw inl outl ( port I/O )  config-l@ config-l! ( PCI )\n");
		return;
	}
	if (!strncmp(t, "line", 4)) {
		printf("^B/^F or arrows move, ^A/^E start/end, ^D delete, ^U erase line,\n^P/^N or up/down history\n");
		return;
	}
	printf("Arithmetic: + - * / mod /mod negate abs min max and or xor lshift rshift\n");
	printf("Output: . u. .h .d .s .r emit type cr space spaces hex decimal\n");
	printf("Definitions: : ; variable constant value create does> defer is to see words\n");
}

struct cmd { const char *name; code_fn fn; };
static const struct cmd cmds[] = {
	{ "banner", c_banner }, { "boot", c_boot }, { "reset-all", c_reset }, { "reset", c_reset },
	{ "power-off", c_poweroff }, { "printenv", c_printenv }, { "setenv", c_setenv },
	{ "set-default", c_setdefault }, { "set-defaults", c_setdefaults }, { "devalias", c_devalias },
	{ "nvalias", c_devalias }, { "show-devs", c_showdevs }, { "show-disks", c_showdisks },
	{ "probe-ide", c_probedisks }, { "probe-scsi", c_probedisks }, { "probe-scsi-all", c_probedisks },
	{ "probe-sata", c_probesata }, { "probe-nvme", c_probedisks }, { "probe-pci", c_probepci },
	{ "probe-pci-slot", c_probepci }, { ".version", c_version }, { ".speed", c_speed },
	{ ".enet-addr", c_enet }, { ".idprom", c_idprom }, { ".memory", c_memory },
	{ "module-info", c_moduleinfo }, { "watch-clock", c_watchclock }, { "time&date", c_timedate },
	{ "test", c_test }, { "test-all", c_testall }, { "load", c_load }, { "go", c_go },
	{ "dir", c_dir }, { "cat", c_cat }, { ".entries", c_entries }, { "cbfs-list", c_cbfslist },
	{ "cbfs-boot", c_cbfsboot }, { "dev", c_dev }, { "cd", c_dev }, { "ls", c_ls }, { "pwd", c_pwd },
	{ ".properties", c_props }, { "nvrun", c_nvramrc }, { "config-l@", c_cfg }, { "config-l!", c_cfgw },
	{ "config-b@", c_cfgb }, { "config-w@", c_cfgww }, { "help", c_help }, { "page", c_screenclear },
};

void obp_words_init(void)
{
	for (u32 i = 0; i < ARRAY_SIZE(cmds); i++) forth_add_prim(cmds[i].name, cmds[i].fn, 0, NULL);
}
