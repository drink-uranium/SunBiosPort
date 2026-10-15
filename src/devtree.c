/* A device tree in the IEEE 1275 style, built from what was probed. */
#include "forth.h"
#include "console.h"
#include "platform.h"
#include "pci.h"
#include "blockdev.h"

#define MAXNODES 192

struct node {
	char path[160];
	const char *type;
	struct pci_dev *pci;
	struct blockdev *bd;
};

static struct node nodes[MAXNODES];
static int nnodes;
static char cur[160] = "/";

static struct node *lookup(const char *path)
{
	for (int i = 0; i < nnodes; i++)
		if (!strcmp(nodes[i].path, path)) return &nodes[i];
	return NULL;
}

static struct node *add(const char *path, const char *type)
{
	struct node *n = lookup(path);
	if (n) return n;
	/* make sure the parents exist */
	char parent[160];
	strlcpy(parent, path, sizeof(parent));
	char *sl = strrchr(parent, '/');
	if (sl && sl != parent) { *sl = 0; add(parent, NULL); }
	if (nnodes >= MAXNODES) return NULL;
	n = &nodes[nnodes++];
	memset(n, 0, sizeof(*n));
	strlcpy(n->path, path, sizeof(n->path));
	n->type = type;
	return n;
}

void devtree_init(void)
{
	nnodes = 0;
	add("/", NULL);
	add("/openprom", NULL);
	add("/options", NULL);
	add("/aliases", NULL);
	add("/chosen", NULL);
	add("/packages", NULL);
	add("/memory@0", "memory");
	add("/cpus/cpu@0", "cpu");
	add("/pci@0,0", "pci");
	for (int i = 0; i < pci_ndevs; i++) {
		struct node *n = add(pci_devs[i].path, NULL);
		if (n) n->pci = &pci_devs[i];
	}
	for (int i = 0; i < nbdevs; i++) {
		struct node *n = add(bdevs[i]->path, bdevs[i]->type == BDEV_CDROM ? "block" : "block");
		if (n) n->bd = bdevs[i];
	}
	add("/isa/serial@i3f8", "serial");
	add("/isa/8042/keyboard", "keyboard");
	if (fb_active()) add("/display", "display");
}

static int depth(const char *p)
{
	int d = 0;
	for (; *p; p++) if (*p == '/') d++;
	return d;
}

void show_devs(const char *root)
{
	const char *r = (root && *root) ? root : "/";
	size_t rl = strlen(r);
	for (int i = 1; i < nnodes; i++) {
		const char *p = nodes[i].path;
		if (rl > 1 && (strncmp(p, r, rl) || (p[rl] && p[rl] != '/'))) continue;
		printf("%s\n", p);
	}
}

void devtree_ls(void)
{
	size_t cl = strlen(cur);
	int cd = cl == 1 ? 0 : depth(cur);
	for (int i = 1; i < nnodes; i++) {
		const char *p = nodes[i].path;
		if (cl > 1 && (strncmp(p, cur, cl) || p[cl] != '/')) continue;
		if (depth(p) != cd + 1) continue;
		printf("%s\n", strrchr(p, '/') + 1);
	}
}

void devtree_dev(const char *path)
{
	char full[160];
	if (!path || !*path) { strlcpy(cur, "/", sizeof(cur)); return; }
	if (!strcmp(path, "..")) {
		char *sl = strrchr(cur, '/');
		if (sl && sl != cur) *sl = 0;
		else strlcpy(cur, "/", sizeof(cur));
		return;
	}
	if (path[0] == '/') strlcpy(full, path, sizeof(full));
	else snprintf(full, sizeof(full), "%s%s%s", cur, strcmp(cur, "/") ? "/" : "", path);
	if (!lookup(full)) {
		/* allow unit-address-less matches: "pci@0,0/ide" */
		for (int i = 0; i < nnodes; i++) {
			if (!strncmp(nodes[i].path, full, strlen(full)) && nodes[i].path[strlen(full)] == '@') {
				strlcpy(cur, nodes[i].path, sizeof(cur));
				return;
			}
		}
		printf("Device not found: %s\n", path);
		return;
	}
	strlcpy(cur, full, sizeof(cur));
}

void devtree_pwd(void) { printf("%s\n", cur); }

void devtree_props(void)
{
	struct node *n = lookup(cur);
	if (!n) return;
	const char *name = strrchr(n->path, '/') + 1;
	char nm[64];
	strlcpy(nm, *name ? name : "SunBiosPort", sizeof(nm));
	char *at = strchr(nm, '@');
	if (at) *at = 0;
	printf("name                     %s\n", nm);
	if (n->type) printf("device_type              %s\n", n->type);
	if (n->pci) {
		struct pci_dev *p = n->pci;
		printf("vendor-id                %08x\n", p->vendor);
		printf("device-id                %08x\n", p->device);
		printf("revision-id              %08x\n", p->rev);
		printf("class-code               %08x\n", (p->class << 16) | (p->subclass << 8) | p->progif);
		printf("compatible               pci%x,%x\n", p->vendor, p->device);
		printf("reg                      %08x 00000000 00000000 00000000 00000000\n",
		       (p->bus << 16) | (p->dev << 11) | (p->fn << 8));
		for (int i = 0; i < 6; i++)
			if (p->bar[i]) printf("assigned-address[%d]      %08x\n", i, p->bar[i]);
	}
	if (n->bd) {
		printf("model                    %s\n", n->bd->model);
		printf("device_type              %s\n", n->bd->type == BDEV_CDROM ? "block (cdrom)" : "block");
		printf("block-size               %x\n", n->bd->sector_size);
		printf("#blocks                  %llx\n", n->bd->nsectors);
	}
	if (!strcmp(n->path, "/")) {
		char model[96];
		extern void obp_cpu_model(char *, size_t);
		obp_cpu_model(model, sizeof(model));
		printf("banner-name              %s\n", model);
		printf("compatible               i86pc\n");
	}
	if (!strcmp(n->path, "/memory@0"))
		printf("reg                      %llx bytes available\n", sysinfo.total_ram);
}
