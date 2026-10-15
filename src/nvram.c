/*
 * NVRAM configuration variables (printenv / setenv / set-defaults) and
 * device aliases, following OpenBoot 2.x. When built with NVRAM_CMOS=1 the
 * most important variables persist in the second CMOS bank (ports
 * 0x72/0x73, bytes 0x80-0xff); otherwise settings last until reset.
 */
#include "nvram.h"
#include "console.h"
#include "blockdev.h"
#include "platform.h"

struct nvvar { const char *name; const char *def; char val[256]; };

static struct nvvar vars[] = {
	{ "auto-boot?", "true" },
	{ "boot-device", "disk cdrom" },
	{ "boot-file", "" },
	{ "boot-entry", "-1" },
	{ "diag-switch?", "false" },
	{ "diag-device", "disk cdrom" },
	{ "diag-file", "" },
	{ "input-device", "keyboard" },
	{ "output-device", "screen" },
	{ "ttya-mode", "115200,8,n,1,-" },
	{ "screen-#columns", "80" },
	{ "screen-#rows", "34" },
	{ "oem-banner?", "false" },
	{ "oem-banner", "" },
	{ "oem-logo?", "false" },
	{ "use-nvramrc?", "false" },
	{ "nvramrc", "" },
	{ "legacy-fallback?", "true" },
	{ "load-base", "1000000" },
	{ "local-mac-address?", "false" },
	{ "selftest-#megs", "1" },
	{ "fcode-debug?", "false" },
	{ "security-mode", "none" },
	{ "keyboard-click?", "false" },
	{ "ansi-terminal?", "true" },
	{ "silent-mode?", "false" },
};

static bool was_reset;

static struct nvvar *find(const char *n)
{
	for (u32 i = 0; i < ARRAY_SIZE(vars); i++)
		if (!strcmp(vars[i].name, n)) return &vars[i];
	return NULL;
}

const char *nv_get(const char *n) { struct nvvar *v = find(n); return v ? v->val : ""; }
bool nv_get_bool(const char *n) { return !strcmp(nv_get(n), "true"); }
long nv_get_int(const char *n)
{
	const char *s = nv_get(n);
	bool neg = *s == '-';
	long v = strtoul(neg ? s + 1 : s, NULL, 16);
	return neg ? -v : v;
}

int nv_set(const char *n, const char *val)
{
	struct nvvar *v = find(n);
	if (!v) return -1;
	if (n[strlen(n) - 1] == '?' && strcmp(val, "true") && strcmp(val, "false")) return -2;
	strlcpy(v->val, val, sizeof(v->val));
	nv_commit();
	return 0;
}

int nv_set_default(const char *n)
{
	struct nvvar *v = find(n);
	if (!v) return -1;
	strlcpy(v->val, v->def, sizeof(v->val));
	nv_commit();
	return 0;
}

void nv_set_defaults(void)
{
	for (u32 i = 0; i < ARRAY_SIZE(vars); i++) strlcpy(vars[i].val, vars[i].def, sizeof(vars[i].val));
	nv_commit();
}

void nv_print(const char *n)
{
	if (n && *n) {
		struct nvvar *v = find(n);
		if (!v) { printf("Unknown option: %s\n", n); return; }
		printf("%s = %s\n", v->name, v->val);
		return;
	}
	printf("Parameter Name      Value                          Default Value\n\n");
	for (u32 i = 0; i < ARRAY_SIZE(vars); i++) {
		const char *val = vars[i].val, *def = vars[i].def;
		if (!strcmp(vars[i].name, "nvramrc")) { val = val[0] ? "(script)" : ""; def = ""; }
		printf("%-19s %-30s %s\n", vars[i].name, val, def);
	}
}

/* ---------------- CMOS persistence ---------------- */
#ifdef NVRAM_CMOS
#define CB 0x80
static u8 cmos_r(u8 i) { outb(0x72, i); return inb(0x73); }
static void cmos_w(u8 i, u8 v) { outb(0x72, i); outb(0x73, v); }

static void pack(u8 *b)
{
	memset(b, 0, 128);
	b[0] = 'O'; b[1] = 'B';
	b[2] = (nv_get_bool("auto-boot?") << 0) | (nv_get_bool("diag-switch?") << 1) |
	       (nv_get_bool("legacy-fallback?") << 2) | (nv_get_bool("oem-banner?") << 3) |
	       (nv_get_bool("use-nvramrc?") << 4);
	b[3] = (s8)nv_get_int("boot-entry");
	strlcpy((char *)b + 4, nv_get("boot-device"), 48);
	strlcpy((char *)b + 52, nv_get("boot-file"), 40);
	strlcpy((char *)b + 92, nv_get("oem-banner"), 34);
	u8 sum = 0;
	for (int i = 0; i < 127; i++) sum += b[i];
	b[127] = ~sum;
}
#endif

void nv_commit(void)
{
#ifdef NVRAM_CMOS
	u8 b[128];
	pack(b);
	for (int i = 0; i < 128; i++) cmos_w(CB + i, b[i]);
#endif
}

void nv_init(void)
{
	for (u32 i = 0; i < ARRAY_SIZE(vars); i++) strlcpy(vars[i].val, vars[i].def, sizeof(vars[i].val));
	was_reset = true;
#ifdef NVRAM_CMOS
	u8 b[128], sum = 0;
	for (int i = 0; i < 128; i++) b[i] = cmos_r(CB + i);
	for (int i = 0; i < 127; i++) sum += b[i];
	if (b[0] == 'O' && b[1] == 'B' && b[127] == (u8)~sum) {
		was_reset = false;
		nv_set("auto-boot?", (b[2] & 1) ? "true" : "false");
		nv_set("diag-switch?", (b[2] & 2) ? "true" : "false");
		nv_set("legacy-fallback?", (b[2] & 4) ? "true" : "false");
		nv_set("oem-banner?", (b[2] & 8) ? "true" : "false");
		nv_set("use-nvramrc?", (b[2] & 16) ? "true" : "false");
		char t[16];
		snprintf(t, sizeof(t), "%d", (s8)b[3]);
		nv_set("boot-entry", t);
		b[51] = b[91] = b[126] = 0;
		nv_set("boot-device", (char *)b + 4);
		nv_set("boot-file", (char *)b + 52);
		nv_set("oem-banner", (char *)b + 92);
	}
#endif
}

bool nv_was_reset(void) { return was_reset; }

/* ---------------- device aliases ---------------- */
struct alias { char name[32]; char path[160]; };
static struct alias aliases[48];
static int naliases;

const char *devalias_get(const char *name)
{
	for (int i = 0; i < naliases; i++)
		if (!strcmp(aliases[i].name, name)) return aliases[i].path;
	return NULL;
}

void devalias_set(const char *name, const char *path)
{
	for (int i = 0; i < naliases; i++)
		if (!strcmp(aliases[i].name, name)) { strlcpy(aliases[i].path, path, 160); return; }
	if (naliases < 48) {
		strlcpy(aliases[naliases].name, name, 32);
		strlcpy(aliases[naliases].path, path, 160);
		naliases++;
	}
}

void devalias_print(const char *name)
{
	for (int i = 0; i < naliases; i++)
		if (!name || !*name || !strcmp(aliases[i].name, name))
			printf("%-15s %s\n", aliases[i].name, aliases[i].path);
}

void devalias_init(void)
{
	bool disk = false, cd = false;
	for (int i = 0; i < nbdevs; i++) {
		struct blockdev *bd = bdevs[i];
		if (bd->type == BDEV_DISK && !disk) { devalias_set("disk", bd->path); disk = true; }
		if (bd->type == BDEV_CDROM && !cd) { devalias_set("cdrom", bd->path); cd = true; }
		devalias_set(bd->alias, bd->path);
	}
	if (sysinfo.have_mac) devalias_set("net", sysinfo.mac_dev);
	devalias_set("ttya", "/isa/serial@i3f8");
	devalias_set("screen", "/display");
	devalias_set("keyboard", "/isa/8042/keyboard");
}
