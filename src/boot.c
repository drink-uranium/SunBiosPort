/*
 * The OpenBoot "boot" command for x86: device specifier resolution,
 * filesystem search, boot loader configuration files (GRUB legacy
 * menu.lst as used by Solaris 10 / OpenSolaris, GRUB 2 grub.cfg as used by
 * Solaris 11, syslinux/isolinux), and fallback to a legacy BIOS.
 */
#include "boot.h"
#include "platform.h"
#include "console.h"
#include "nvram.h"
#include "pci.h"

#define MAXVOL 24

struct bootdev {
	bool net, cbfs, legacy;
	struct blockdev *bd;
	int part;		/* -1 = search */
	char path[160];
	char cbfs_name[64];
};

static bool cpu_lm(void)
{
	u32 a, b, c, d;
	cpuid(0x80000000, 0, &a, &b, &c, &d);
	if (a < 0x80000001) return false;
	cpuid(0x80000001, 0, &a, &b, &c, &d);
	return (d >> 29) & 1;
}

static int resolve(const char *spec, struct bootdev *bdv, int depth)
{
	char name[160], args[64] = "";
	memset(bdv, 0, sizeof(*bdv));
	bdv->part = -1;
	strlcpy(name, spec, sizeof(name));
	char *colon = strrchr(name, ':');
	if (colon && !strchr(colon, '/')) { strlcpy(args, colon + 1, sizeof(args)); *colon = 0; }
	const char *al = devalias_get(name);
	if (al && depth < 4) {
		char full[200];
		snprintf(full, sizeof(full), "%s%s%s", al, args[0] ? ":" : "", args);
		int r = resolve(full, bdv, depth + 1);
		return r;
	}
	if (!strcmp(name, "net") || strstr(name, "ethernet")) {
		bdv->net = true;
		strlcpy(bdv->path, spec, sizeof(bdv->path));
		return 0;
	}
	if (!strcmp(name, "legacy") || !strcmp(name, "bios")) {
		bdv->legacy = true;
		strlcpy(bdv->path, name, sizeof(bdv->path));
		return 0;
	}
	if (!strncmp(name, "/cbfs", 5) || !strcmp(name, "cbfs")) {
		bdv->cbfs = true;
		strlcpy(bdv->cbfs_name, args[0] ? args : "img/seabios", sizeof(bdv->cbfs_name));
		snprintf(bdv->path, sizeof(bdv->path), "%s", spec);
		return 0;
	}
	bdv->bd = bdev_find(name);
	if (!bdv->bd) return -1;
	if (args[0] >= 'a' && args[0] <= 'x' && !args[1]) bdv->part = args[0] - 'a';
	else if (isdigit(args[0])) bdv->part = strtoul(args, NULL, 10);
	snprintf(bdv->path, sizeof(bdv->path), "%s%s%s", bdv->bd->path, args[0] ? ":" : "", args);
	return 0;
}

/* ---------------- configuration files ---------------- */
static void finish_config(struct boot_config *c);

static const char *cfg_files[] = {
	"/boot/grub/menu.lst", "/boot/grub/grub.cfg", "/boot/grub2/grub.cfg", "/grub/menu.lst",
	"/grub/grub.cfg", "/boot/grub/grub.conf", "/isolinux/isolinux.cfg", "/boot/isolinux/isolinux.cfg",
	"/isolinux.cfg", "/syslinux/syslinux.cfg", "/boot/syslinux/syslinux.cfg", "/syslinux.cfg",
	"/extlinux/extlinux.conf", "/boot/extlinux/extlinux.conf", "/extlinux.conf",
};

struct vars { char name[16][32]; char val[16][256]; int n; };

static const char *var_get(struct vars *v, const char *n, size_t l)
{
	for (int i = v->n - 1; i >= 0; i--)
		if (strlen(v->name[i]) == l && !strncmp(v->name[i], n, l)) return v->val[i];
	if (l == 6 && !strncmp(n, "ISADIR", 6)) return cpu_lm() ? "amd64" : "";
	if (l == 9 && !strncmp(n, "multiboot", 9)) return "multiboot";
	if (l == 6 && !strncmp(n, "module", 6)) return "module";
	return "";
}

static void var_set(struct vars *v, const char *n, const char *val)
{
	for (int i = 0; i < v->n; i++)
		if (!strcmp(v->name[i], n)) { strlcpy(v->val[i], val, 256); return; }
	if (v->n < 16) {
		strlcpy(v->name[v->n], n, 32);
		strlcpy(v->val[v->n], val, 256);
		v->n++;
	}
}

static void expand(struct vars *v, const char *in, char *out, size_t max)
{
	size_t o = 0;
	while (*in && o + 1 < max) {
		if (*in == '$') {
			const char *s = in + 1, *e;
			if (*s == '{') { s++; e = strchr(s, '}'); if (!e) e = s + strlen(s); }
			else { e = s; while (isalpha(*e) || isdigit(*e) || *e == '_') e++; }
			const char *val = var_get(v, s, e - s);
			while (*val && o + 1 < max) out[o++] = *val++;
			in = (*e == '}') ? e + 1 : e;
			continue;
		}
		if (*in == '"' || *in == '\'') { in++; continue; }
		out[o++] = *in++;
	}
	out[o] = 0;
}

static char *skip_ws(char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

static char *next_tok(char **p)
{
	char *s = skip_ws(*p);
	if (!*s) { *p = s; return NULL; }
	char *e = s;
	while (*e && *e != ' ' && *e != '\t') e++;
	if (*e) *e++ = 0;
	*p = e;
	return s;
}

static void strip_dev(char *path)
{
	/* "(hd0,0)/boot/x" -> "/boot/x" */
	if (path[0] == '(') {
		char *e = strchr(path, ')');
		if (e) memmove(path, e + 1, strlen(e + 1) + 1);
	}
}

static struct boot_entry *new_entry(struct boot_config *c, const char *title)
{
	if (c->nentries >= 32) return NULL;
	struct boot_entry *e = &c->e[c->nentries++];
	memset(e, 0, sizeof(*e));
	strlcpy(e->title, title, sizeof(e->title));
	return e;
}

static void set_kernel(struct boot_entry *e, char *rest, bool mb_includes_path)
{
	char *p = rest, *t;
	while ((t = next_tok(&p)) && t[0] == '-' && t[1] == '-')
		;
	if (!t) return;
	strlcpy(e->kernel, t, sizeof(e->kernel));
	strip_dev(e->kernel);
	p = skip_ws(p);
	if (mb_includes_path) snprintf(e->kargs, sizeof(e->kargs), "%s%s%s", e->kernel, *p ? " " : "", p);
	else strlcpy(e->kargs, p, sizeof(e->kargs));
	e->chain = false;
}

static void add_module(struct boot_entry *e, char *rest, bool includes_path)
{
	char *p = rest, *t;
	while ((t = next_tok(&p)) && t[0] == '-' && t[1] == '-')
		;
	if (!t || e->nmods >= MAX_MODS) return;
	strlcpy(e->mods[e->nmods], t, 256);
	strip_dev(e->mods[e->nmods]);
	p = skip_ws(p);
	if (includes_path) snprintf(e->modargs[e->nmods], 256, "%s%s%s", e->mods[e->nmods], *p ? " " : "", p);
	else strlcpy(e->modargs[e->nmods], p, 256);
	e->nmods++;
}

static void add_initrds(struct boot_entry *e, char *rest, char sep)
{
	char *p = rest;
	while (*p && e->ninitrd < 4) {
		while (*p == sep || *p == ' ' || *p == '\t') p++;
		char *st = p;
		while (*p && *p != sep && *p != ' ' && *p != '\t') p++;
		if (p == st) break;
		size_t l = MIN((size_t)(p - st), (size_t)255);
		memcpy(e->initrd[e->ninitrd], st, l);
		e->initrd[e->ninitrd][l] = 0;
		strip_dev(e->initrd[e->ninitrd]);
		e->ninitrd++;
	}
}

static void parse_menulst(struct boot_config *c, char *text)
{
	struct vars v = { .n = 0 };
	struct boot_entry *e = NULL;
	for (char *line = text; line && *line;) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = 0;
		char *s = skip_ws(line);
		char *cr = strchr(s, '\r'); if (cr) *cr = 0;
		line = nl ? nl + 1 : NULL;
		if (!*s || *s == '#') continue;
		char kw[32];
		int i = 0;
		while (*s && *s != ' ' && *s != '\t' && *s != '=' && i < 31) kw[i++] = *s++;
		kw[i] = 0;
		s = skip_ws(s);
		if (*s == '=') s = skip_ws(s + 1);
		char buf[768];
		expand(&v, s, buf, sizeof(buf));
		if (!strcmp(kw, "default")) c->def = isdigit(buf[0]) ? (int)strtoul(buf, NULL, 10) : 0;
		else if (!strcmp(kw, "title")) e = new_entry(c, buf);
		else if (!e) continue;
		else if (!strcmp(kw, "kernel") || !strcmp(kw, "kernel$")) set_kernel(e, buf, true);
		else if (!strcmp(kw, "module") || !strcmp(kw, "module$")) add_module(e, buf, true);
		else if (!strcmp(kw, "initrd")) add_initrds(e, buf, ' ');
		else if (!strcmp(kw, "chainloader")) e->chain = true;
	}
}

static void parse_grubcfg(struct boot_config *c, char *text)
{
	struct vars v = { .n = 0 };
	struct boot_entry *e = NULL;
	for (char *line = text; line && *line;) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = 0;
		char *s = skip_ws(line);
		char *cr = strchr(s, '\r'); if (cr) *cr = 0;
		line = nl ? nl + 1 : NULL;
		if (!*s || *s == '#') continue;
		if (!strncmp(s, "menuentry", 9)) {
			char *q = s + 9, title[96] = "";
			q = skip_ws(q);
			if (*q == '"' || *q == '\'') {
				char qc = *q++;
				char *qe = strchr(q, qc);
				if (qe) { *qe = 0; strlcpy(title, q, sizeof(title)); }
			} else {
				char *t = next_tok(&q);
				if (t) strlcpy(title, t, sizeof(title));
			}
			e = new_entry(c, title);
			continue;
		}
		if (*s == '}') { e = NULL; continue; }
		char buf[768];
		expand(&v, s, buf, sizeof(buf));
		char *p = buf;
		char *cmd = next_tok(&p);
		if (!cmd) continue;
		if (!strcmp(cmd, "set")) {
			char *eq = strchr(p, '=');
			if (!eq) continue;
			*eq = 0;
			char *n = skip_ws(p);
			char *val = eq + 1;
			/* keep only the first word of unquoted values */
			char *ve = val + strlen(val);
			while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
			var_set(&v, n, val);
			if (!strcmp(n, "default") && isdigit(val[0])) c->def = strtoul(val, NULL, 10);
			continue;
		}
		if (!e) continue;
		if (!strcmp(cmd, "linux") || !strcmp(cmd, "linux16") || !strcmp(cmd, "linuxefi") || !strcmp(cmd, "kernel"))
			set_kernel(e, p, false);
		else if (strstr(cmd, "multiboot"))
			set_kernel(e, p, false);
		else if (!strncmp(cmd, "initrd", 6))
			add_initrds(e, p, ' ');
		else if (strstr(cmd, "module"))
			add_module(e, p, false);
		else if (!strcmp(cmd, "chainloader"))
			e->chain = true;
	}
}

static void parse_syslinux(struct boot_config *c, char *text, const char *dir)
{
	struct boot_entry *e = NULL;
	char deflabel[64] = "";
	for (char *line = text; line && *line;) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = 0;
		char *s = skip_ws(line);
		char *cr = strchr(s, '\r'); if (cr) *cr = 0;
		line = nl ? nl + 1 : NULL;
		if (!*s || *s == '#') continue;
		char *p = s;
		char *kw = next_tok(&p);
		if (!kw) continue;
		for (char *k = kw; *k; k++) *k = tolower(*k);
		p = skip_ws(p);
		if (!strcmp(kw, "default")) { strlcpy(deflabel, p, sizeof(deflabel)); continue; }
		if (!strcmp(kw, "label")) { e = new_entry(c, p); continue; }
		if (!e) continue;
		if (!strcmp(kw, "kernel") || !strcmp(kw, "linux")) {
			char path[256];
			if (p[0] == '/') strlcpy(path, p, sizeof(path));
			else snprintf(path, sizeof(path), "%s/%s", dir, p);
			strlcpy(e->kernel, path, sizeof(e->kernel));
		} else if (!strcmp(kw, "initrd")) {
			add_initrds(e, p, ',');
		} else if (!strcmp(kw, "append")) {
			if (strstr(e->kernel, "mboot.c32")) {
				/* multiboot via mboot.c32: "kernel args --- module args --- ..." */
				char *part = p;
				bool first = true;
				while (part) {
					char *sep = strstr(part, " --- ");
					if (sep) *sep = 0;
					char tmp[512];
					strlcpy(tmp, skip_ws(part), sizeof(tmp));
					char *t = tmp, *file = next_tok(&t);
					if (file) {
						char path[256], line2[768];
						if (file[0] == '/') strlcpy(path, file, sizeof(path));
						else snprintf(path, sizeof(path), "%s/%s", dir, file);
						snprintf(line2, sizeof(line2), "%s %s", path, skip_ws(t));
						if (first) set_kernel(e, line2, true);
						else add_module(e, line2, true);
					}
					first = false;
					part = sep ? sep + 5 : NULL;
				}
			} else {
				strlcpy(e->kargs, p, sizeof(e->kargs));
				char *ir = strstr(p, "initrd=");
				if (ir) {
					char tmp[512];
					strlcpy(tmp, ir + 7, sizeof(tmp));
					char *sp = strchr(tmp, ' ');
					if (sp) *sp = 0;
					int before = e->ninitrd;
					add_initrds(e, tmp, ',');
					for (int i = before; i < e->ninitrd; i++) {
						if (e->initrd[i][0] != '/') {
							char q[256];
							snprintf(q, sizeof(q), "%s/%s", dir, e->initrd[i]);
							strlcpy(e->initrd[i], q, 256);
						}
					}
				}
			}
		}
	}
	for (int i = 0; i < c->nentries; i++)
		if (!strcasecmp(c->e[i].title, deflabel)) c->def = i;
	/* initrd paths given via INITRD may be relative too */
	for (int i = 0; i < c->nentries; i++)
		for (int j = 0; j < c->e[i].ninitrd; j++)
			if (c->e[i].initrd[j][0] != '/') {
				char q[256];
				snprintf(q, sizeof(q), "%s/%s", dir, c->e[i].initrd[j]);
				strlcpy(c->e[i].initrd[j], q, 256);
			}
}

static int load_config(struct fs *fs, struct boot_config *c)
{
	memset(c, 0, sizeof(*c));
	c->e = zalloc(sizeof(struct boot_entry) * 32);
	if (!c->e) return -1;
	for (u32 i = 0; i < ARRAY_SIZE(cfg_files); i++) {
		struct file f;
		if (fs_open(fs, cfg_files[i], &f) < 0 || f.node.dir || f.size > 256 * 1024) continue;
		char *text = malloc(f.size + 1);
		if (!text) continue;
		int n = fs_read(&f, 0, f.size, text);
		if (n < 0) { free(text); continue; }
		text[n] = 0;
		strlcpy(c->path, cfg_files[i], sizeof(c->path));
		if (strstr(cfg_files[i], "menu.lst") || strstr(cfg_files[i], "grub.conf"))
			parse_menulst(c, text);
		else if (strstr(cfg_files[i], "grub.cfg"))
			parse_grubcfg(c, text);
		else {
			char dir[64];
			strlcpy(dir, cfg_files[i], sizeof(dir));
			*strrchr(dir, '/') = 0;
			parse_syslinux(c, text, dir);
		}
		free(text);
		finish_config(c);
		if (c->nentries) return 0;
	}
	/* Fallback: a Solaris root without a menu */
	static const char *sol[][2] = {
		{ "/platform/i86pc/kernel/amd64/unix", "/platform/i86pc/amd64/boot_archive" },
		{ "/platform/i86pc/kernel/unix", "/platform/i86pc/boot_archive" },
		{ "/platform/i86pc/multiboot", "/platform/i86pc/boot_archive" },
	};
	for (u32 i = 0; i < ARRAY_SIZE(sol); i++) {
		struct file f;
		if (i == 0 && !cpu_lm()) continue;
		if (fs_open(fs, sol[i][0], &f) < 0 || fs_open(fs, sol[i][1], &f) < 0) continue;
		struct boot_entry *e = new_entry(c, "Solaris");
		strlcpy(e->kernel, sol[i][0], sizeof(e->kernel));
		strlcpy(e->kargs, sol[i][0], sizeof(e->kargs));
		strlcpy(e->mods[0], sol[i][1], 256);
		strlcpy(e->modargs[0], sol[i][1], 256);
		e->nmods = 1;
		strlcpy(c->path, "(Solaris default)", sizeof(c->path));
		return 0;
	}
	free(c->e);
	c->e = NULL;
	return -1;
}

/* ---------------- running an entry ---------------- */
static int open_src(struct fs *fs, const char *path, struct src *s, struct file *f)
{
	memset(s, 0, sizeof(*s));
	if (fs_open(fs, path, f) < 0 || f->node.dir) return -1;
	s->file = f;
	s->size = f->size;
	strlcpy(s->name, path, sizeof(s->name));
	return 0;
}

static int legacy_for(struct bootdev *bdv);
static int legacy_for_dev(struct bootdev *bdv, bool explicit);

static int fs_opener(void *ctx, const char *path, struct src *s)
{
	struct file *f = malloc(sizeof(*f));
	if (!f) return -1;
	if (open_src(ctx, path, s, f) < 0) { free(f); return -1; }
	return 0;
}

static int run_entry(src_opener open, void *ctx, struct boot_entry *e, const char *extra,
		     struct bootdev *bdv)
{
	if (e->chain) {
		if (bdv) return legacy_for_dev(bdv, true);
		printf("chainloader is not supported here\n");
		return -1;
	}
	struct src k, mods[MAX_MODS], rd[4];
	const char *modcmd[MAX_MODS];
	phys_reset();
	if (open(ctx, e->kernel, &k) < 0) {
		printf("Can't open %s\n", e->kernel);
		return -1;
	}
	for (int i = 0; i < e->nmods; i++) {
		if (open(ctx, e->mods[i], &mods[i]) < 0) {
			printf("Can't open module %s\n", e->mods[i]);
			return -1;
		}
		modcmd[i] = e->modargs[i];
	}
	for (int i = 0; i < e->ninitrd; i++) {
		if (open(ctx, e->initrd[i], &rd[i]) < 0) {
			printf("Can't open initrd %s\n", e->initrd[i]);
			return -1;
		}
	}
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "%s%s%s", e->kargs, (extra && *extra) ? " " : "", extra ? extra : "");
	return boot_load_image(&k, cmd, mods, modcmd, e->nmods, rd, e->ninitrd);
}

static void finish_config(struct boot_config *c)
{
	int k = 0;
	for (int j = 0; j < c->nentries; j++) {
		if (!c->e[j].kernel[0] && !c->e[j].chain) { if (c->def > k) c->def--; continue; }
		if (k != j) c->e[k] = c->e[j];
		k++;
	}
	c->nentries = k;
	if (c->def < 0 || c->def >= c->nentries) c->def = 0;
}

static int pick_entry(struct boot_config *c)
{
	long want = nv_get_int("boot-entry");
	return (want >= 0 && want < c->nentries) ? (int)want : c->def;
}

/* Boot from configuration text fetched over the network (kind 1 = GRUB
 * menu.lst, 2 = syslinux, 3 = grub.cfg). */
int boot_config_text(char *text, int kind, const char *dir, const char *args, src_opener open, void *ctx)
{
	struct boot_config c;
	memset(&c, 0, sizeof(c));
	c.e = zalloc(sizeof(struct boot_entry) * 32);
	if (!c.e) return -1;
	if (kind == 1) parse_menulst(&c, text);
	else if (kind == 2) parse_syslinux(&c, text, dir);
	else parse_grubcfg(&c, text);
	free(text);
	finish_config(&c);
	if (!c.nentries) { printf("No usable entries in boot configuration\n"); return -1; }
	int idx = pick_entry(&c);
	printf("Entry %d: %s\n", idx, c.e[idx].title);
	/* menu.lst paths on a TFTP server are relative to the server root */
	return run_entry(open, ctx, &c.e[idx], args, NULL);
}

static int scan_volumes(struct bootdev *bdv, struct volume *vols)
{
	int n = part_scan(bdv->bd, vols, MAXVOL);
	if (bdv->part >= 0) {
		/* letters a.. select partitions; with no table, everything is the whole disk */
		int idx = n > 1 ? bdv->part + 1 : 0;
		if (idx >= n) idx = 0;
		vols[0] = vols[idx];
		return 1;
	}
	if (n > 1 && bdv->bd->type == BDEV_DISK) {
		/* partitions first, the raw disk last (for superfloppy media) */
		struct volume w = vols[0];
		memmove(vols, vols + 1, (n - 1) * sizeof(*vols));
		vols[n - 1] = w;
	}
	return n;
}

static int boot_blockdev(struct bootdev *bdv, const char *file, const char *args)
{
	struct volume *vols = malloc(sizeof(struct volume) * MAXVOL);
	struct fs *fs = malloc(sizeof(struct fs));
	if (!vols || !fs) return -1;
	int n = scan_volumes(bdv, vols);
	bool mounted = false;
	for (int i = 0; i < n; i++) {
		if (fs_mount(&vols[i], fs) < 0) continue;
		mounted = true;
		if (file && *file) {
			struct file f;
			struct src k;
			if (open_src(fs, file, &k, &f) < 0) continue;
			phys_reset();
			char cmd[1024];
			snprintf(cmd, sizeof(cmd), "%s", args ? args : "");
			/* Multiboot command lines start with the kernel path */
			char mbcmd[1024];
			snprintf(mbcmd, sizeof(mbcmd), "%s%s%s", file, *cmd ? " " : "", cmd);
			u8 sig[4];
			bool is_linux = src_read(&k, 0x202, 4, sig) == 0 && !memcmp(sig, "HdrS", 4);
			int r = boot_load_image(&k, is_linux ? cmd : mbcmd, NULL, NULL, 0, NULL, 0);
			free(vols); free(fs);
			return r;
		}
		struct boot_config cfg;
		if (load_config(fs, &cfg) < 0) continue;
		int idx = pick_entry(&cfg);
		printf("Using %s, entry %d: %s\n", cfg.path, idx, cfg.e[idx].title);
		int r = run_entry(fs_opener, fs, &cfg.e[idx], args, bdv);
		free(cfg.e);
		free(vols); free(fs);
		return r;
	}
	free(vols); free(fs);
	if (file && *file) {
		printf("Can't open %s on %s\n", file, bdv->path);
		return -1;
	}
	if (!mounted) printf("No recognizable file system on %s\n", bdv->path);
	else printf("No boot configuration found on %s\n", bdv->path);
	if (nv_get_bool("legacy-fallback?")) return legacy_for(bdv);
	return -1;
}

static int bios_drive_for(struct blockdev *bd)
{
	int n = 0;
	for (int i = 0; i < nbdevs; i++) {
		if (bdevs[i] == bd) return 0x80 + n;
		if (bdevs[i]->type == BDEV_DISK) n++;
	}
	return 0x80;
}

static bool has_eltorito(struct blockdev *bd)
{
	struct volume v = { bd, 0, bd->nsectors * bd->sector_size, "" };
	u8 vd[2048];
	if (vol_read(&v, 17 * 2048, 2048, vd) < 0) return false;
	return vd[0] == 0 && !memcmp(vd + 1, "CD001", 5) && !memcmp(vd + 7, "EL TORITO SPECIFICATION", 23);
}

static bool has_mbr(struct blockdev *bd)
{
	u8 *b = malloc(bd->sector_size);
	bool ok = b && bdev_read(bd, 0, 1, b) == 0 && b[510] == 0x55 && b[511] == 0xaa;
	free(b);
	return ok;
}

/* explicit: "boot legacy" / a GRUB chainloader entry; otherwise only devices
 * that actually carry PC boot code are handed over */
static int legacy_for_dev(struct bootdev *bdv, bool explicit)
{
	bool disk = bdv->bd && bdv->bd->type == BDEV_DISK;
	bool cd = bdv->bd && bdv->bd->type == BDEV_CDROM;
	if (!explicit) {
		if (disk && !has_mbr(bdv->bd)) return -1;
		if (cd && !has_eltorito(bdv->bd)) return -1;
		if (!disk && !cd) return -1;
	}
	if (sysinfo.bios_present) {
		if (disk) {
			if (!has_mbr(bdv->bd)) { printf("%s has no boot block\n", bdv->path); return -1; }
			printf("Handing %s to the resident PC BIOS\n", bdv->path);
			return legacy_boot(bios_drive_for(bdv->bd));
		}
		if (!explicit) {
			printf("%s has an El Torito boot image; use  boot legacy  to start it\n", bdv->path);
			printf("from the resident PC BIOS.\n");
			return -1;
		}
		printf("Handing off to the resident PC BIOS bootstrap loader (INT 19h)\n");
		return legacy_boot(-1);
	}
	if (sysinfo.from_coreboot) {
		printf("Starting legacy BIOS payload img/seabios from CBFS\n");
		return cbfs_boot("img/seabios");
	}
	if (explicit) printf("No legacy BIOS available\n");
	return -1;
}

static int legacy_for(struct bootdev *bdv) { return legacy_for_dev(bdv, bdv->legacy); }

static int boot_one(const char *dev, const char *file, const char *args)
{
	struct bootdev bdv;
	if (resolve(dev, &bdv, 0) < 0) {
		printf("Boot device: %s  File and args: %s%s%s\n", dev, file ? file : "", (file && *file) ? " " : "", args ? args : "");
		printf("Can't open boot device\n\n");
		return -1;
	}
	printf("Boot device: %s  File and args: %s%s%s\n", bdv.path, file ? file : "", (file && *file && args && *args) ? " " : "", args ? args : "");
	if (bdv.net) return net_boot(file, args);
	if (bdv.cbfs) return cbfs_boot(bdv.cbfs_name);
	if (bdv.legacy) return legacy_for(&bdv);
	return boot_blockdev(&bdv, file, args);
}

static bool looks_like_device(const char *t)
{
	if (t[0] == '/') return true;
	char name[64];
	strlcpy(name, t, sizeof(name));
	char *c = strchr(name, ':');
	if (c) *c = 0;
	if (devalias_get(name) || bdev_find(name)) return true;
	return !strcmp(name, "net") || !strcmp(name, "cbfs") || !strcmp(name, "legacy") || !strcmp(name, "bios");
}

int boot_command(const char *argstr)
{
	char buf[512], *p = buf;
	strlcpy(buf, argstr ? argstr : "", sizeof(buf));
	p = skip_ws(p);
	char devs[256] = "", file[256] = "", *args;
	bool diag = nv_get_bool("diag-switch?");
	if (*p && *p != '-') {
		char *save = p;
		char *t = next_tok(&p);
		if (looks_like_device(t)) strlcpy(devs, t, sizeof(devs));
		else { p = save; if (t) t[strlen(t)] = ' '; }
	}
	p = skip_ws(p);
	if (*p && *p != '-') {
		char *t = next_tok(&p);
		strlcpy(file, t, sizeof(file));
	}
	args = skip_ws(p);
	if (!devs[0]) strlcpy(devs, nv_get(diag ? "diag-device" : "boot-device"), sizeof(devs));
	if (!file[0] && !*args) strlcpy(file, nv_get(diag ? "diag-file" : "boot-file"), sizeof(file));
	/* boot-device may list several devices; try each in turn */
	char *dp = devs, *d;
	int r = -1;
	while ((d = next_tok(&dp))) {
		r = boot_one(d, file, args);
		if (r == 0) break;
	}
	return r;
}

/* .entries <dev>: list the boot configuration of a device */
int boot_entries_command(const char *dev)
{
	struct bootdev bdv;
	if (!dev || !*dev) dev = "cdrom";
	if (resolve(dev, &bdv, 0) < 0 || !bdv.bd) { printf("Can't open %s\n", dev); return -1; }
	struct volume vols[MAXVOL];
	struct fs fs;
	int n = scan_volumes(&bdv, vols);
	for (int i = 0; i < n; i++) {
		if (fs_mount(&vols[i], &fs) < 0) continue;
		struct boot_config cfg;
		if (load_config(&fs, &cfg) < 0) continue;
		printf("%s (%s, %s):\n", cfg.path, vols[i].desc, fs.ops->name);
		for (int j = 0; j < cfg.nentries; j++) {
			struct boot_entry *e = &cfg.e[j];
			printf(" %c%2d  %s\n", j == cfg.def ? '*' : ' ', j, e->title);
			if (e->chain) { printf("        chainloader\n"); continue; }
			printf("        kernel %s  [%s]\n", e->kernel, e->kargs);
			for (int m = 0; m < e->nmods; m++) printf("        module %s\n", e->mods[m]);
			for (int m = 0; m < e->ninitrd; m++) printf("        initrd %s\n", e->initrd[m]);
		}
		free(cfg.e);
		return 0;
	}
	printf("No boot configuration found on %s\n", bdv.path);
	return -1;
}

/* ---------------- dir / load / go ---------------- */
static char last_path[256];

static int open_spec(const char *spec, struct fs *fs, struct file *f, char *rest, size_t restlen)
{
	/* "<device> <path>" or "<device>:<part>,<path>" */
	char buf[256], *p = buf;
	strlcpy(buf, spec, sizeof(buf));
	char *dev = next_tok(&p);
	char *path = next_tok(&p);
	if (rest) strlcpy(rest, skip_ws(p), restlen);
	if (!dev) return -1;
	if (!path) {
		char *comma = strchr(dev, ',');
		if (comma && comma[1] == '/') { *comma = 0; path = comma + 1; }
		else path = "/";
	}
	strlcpy(last_path, path, sizeof(last_path));
	struct bootdev bdv;
	if (resolve(dev, &bdv, 0) < 0 || !bdv.bd) { printf("Can't open device %s\n", dev); return -1; }
	struct volume vols[MAXVOL];
	int n = scan_volumes(&bdv, vols);
	for (int i = 0; i < n; i++) {
		if (fs_mount(&vols[i], fs) < 0) continue;
		if (fs_open(fs, path, f) == 0) return 0;
	}
	printf("Can't open %s on %s\n", path, dev);
	return -1;
}

struct dir_ctx { int count; };

static void dir_print(void *arg, const char *name, const struct fnode *n)
{
	struct dir_ctx *c = arg;
	c->count++;
	if (n->dir) printf("        <dir>  %s/\n", name);
	else printf("%13llu  %s%s\n", n->size, name, n->link ? "@" : "");
}

int dir_command(const char *spec)
{
	struct fs fs;
	struct file f;
	if (open_spec(spec, &fs, &f, NULL, 0) < 0) return -1;
	if (!f.node.dir) { dir_print(&(struct dir_ctx){ 0 }, spec, &f.node); return 0; }
	struct dir_ctx c = { 0 };
	fs.ops->readdir(&fs, &f.node, dir_print, &c);
	printf("%d entries (%s)\n", c.count, fs.ops->name);
	return 0;
}

int cat_command(const char *spec)
{
	struct fs fs;
	struct file f;
	if (open_spec(spec, &fs, &f, NULL, 0) < 0) return -1;
	char buf[512];
	for (u64 off = 0; off < f.size; off += sizeof(buf)) {
		int n = fs_read(&f, off, sizeof(buf), buf);
		if (n <= 0) break;
		for (int i = 0; i < n; i++) console_putc(buf[i] == '\n' ? '\n' : buf[i]);
		console_flush();
	}
	return 0;
}

static struct src loaded_src;
static char loaded_args[256];

int load_command(const char *spec, uintptr_t addr, u32 *size)
{
	struct fs fs;
	struct file f;
	char rest[256];
	if (open_spec(spec, &fs, &f, rest, sizeof(rest)) < 0) return -1;
	if (f.node.dir) { printf("Is a directory\n"); return -1; }
	if (addr + f.size > mem_top_below(0xffffffffull) || !phys_free_range(addr, addr + f.size)) {
		uintptr_t hs, he;
		heap_bounds(&hs, &he);
		if (!(addr >= hs && addr + f.size <= he)) {
			printf("load-base %lx cannot hold %llu bytes\n", addr, f.size);
			return -1;
		}
	}
	if (fs_read(&f, 0, f.size, (void *)addr) != (int)f.size) { printf("Read error\n"); return -1; }
	*size = f.size;
	memset(&loaded_src, 0, sizeof(loaded_src));
	loaded_src.mem = (const u8 *)addr;
	loaded_src.size = f.size;
	strlcpy(loaded_src.name, last_path, sizeof(loaded_src.name));
	strlcpy(loaded_args, rest, sizeof(loaded_args));
	return 0;
}

int go_command(uintptr_t addr, u32 size)
{
	if (loaded_src.mem != (const u8 *)addr || !size) {
		memset(&loaded_src, 0, sizeof(loaded_src));
		loaded_src.mem = (const u8 *)addr;
		loaded_src.size = size;
		strlcpy(loaded_src.name, "memory", sizeof(loaded_src.name));
	}
	phys_reset();
	/* the image lives at load-base; don't let the loader place things over it */
	phys_reserve(addr, addr + size);
	/* Multiboot kernels expect their own path first on the command line */
	char cmd[512];
	bool is_linux = size > 0x206 && !memcmp(loaded_src.mem + 0x202, "HdrS", 4);
	if (is_linux) strlcpy(cmd, loaded_args, sizeof(cmd));
	else snprintf(cmd, sizeof(cmd), "%s%s%s", loaded_src.name, loaded_args[0] ? " " : "", loaded_args);
	return boot_load_image(&loaded_src, cmd, NULL, NULL, 0, NULL, 0);
}

/* ---------------- legacy BIOS hand-off ---------------- */
extern u8 realmode_blob[], realmode_blob_end[];

int legacy_boot(int drive)
{
	memcpy((void *)0x6000, realmode_blob, realmode_blob_end - realmode_blob);
	*(u32 *)0x5e00 = drive < 0 ? 2 : 1;
	*(u8 *)0x5e04 = drive < 0 ? 0 : drive;
	console_flush();
	if (vga_text_saved()) vga_restore_text();
	fb_shutdown();
	((void (*)(void))0x6000)();
	return -1;
}
