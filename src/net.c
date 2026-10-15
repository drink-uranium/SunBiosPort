/*
 * "boot net": Intel 8254x/8257x (e1000) driver, ARP, DHCP/BOOTP and TFTP.
 *
 * Mirrors the OpenBoot network boot flow: get an address, then fetch the
 * boot program by TFTP. A GRUB style menu (menu.lst.01<MAC>, as used by
 * Solaris PXE/jumpstart servers) or pxelinux.cfg is honoured when present.
 */
#include "boot.h"
#include "platform.h"
#include "console.h"
#include "nvram.h"
#include "pci.h"

#define NRX 32
#define NTX 8
#define BUFSZ 2048

struct rxd { u64 addr; u16 len, csum; u8 status, err; u16 special; } PACKED;
struct txd { u64 addr; u16 len; u8 cso, cmd, status, css; u16 special; } PACKED;

static uintptr_t mmio;
static struct rxd *rx;
static struct txd *tx;
static u8 *rxbuf, *txbuf;
static u32 rx_tail, tx_tail;
static u8 mymac[6];
static u32 myip, server_ip, gateway_ip, netmask;
static bool nic_up;
static struct pci_dev *nic;

static u32 rd(u32 r) { return read32(mmio + r); }
static void wr(u32 r, u32 v) { write32(mmio + r, v); }

static bool is_e1000(struct pci_dev *p)
{
	if (p->vendor != 0x8086 || p->class != 2) return false;
	static const u16 ids[] = { 0x100e, 0x100f, 0x1004, 0x100c, 0x1010, 0x1011, 0x1015, 0x1017, 0x1019,
				   0x101e, 0x1026, 0x1075, 0x1076, 0x1079, 0x107c, 0x108b, 0x108c, 0x109a,
				   0x10d3, 0x10b9, 0x10f6, 0x150c };
	for (u32 i = 0; i < ARRAY_SIZE(ids); i++) if (p->device == ids[i]) return true;
	return false;
}

/* Read the MAC without bringing the interface up (for the banner). */
int net_probe(void)
{
	for (int i = 0; i < pci_ndevs; i++) {
		struct pci_dev *p = &pci_devs[i];
		if (is_e1000(p)) {
			uintptr_t b = pci_bar_mem(p, 0);
			if (!b) continue;
			pci_enable(p);
			u32 lo = read32(b + 0x5400), hi = read32(b + 0x5404);
			if (!(hi & 0x80000000u)) {
				/* EEPROM via EERD */
				for (int w = 0; w < 3; w++) {
					write32(b + 0x14, 1 | (w << 8));
					u32 v = 0;
					for (int t = 0; t < 10000; t++) { v = read32(b + 0x14); if (v & 0x10) break; udelay(5); }
					u16 word = v >> 16;
					sysinfo.mac[w * 2] = word; sysinfo.mac[w * 2 + 1] = word >> 8;
				}
			} else {
				for (int k = 0; k < 4; k++) sysinfo.mac[k] = lo >> (k * 8);
				sysinfo.mac[4] = hi; sysinfo.mac[5] = hi >> 8;
			}
			nic = p;
			sysinfo.have_mac = true;
			snprintf(sysinfo.mac_dev, sizeof(sysinfo.mac_dev), "%s", p->path);
			return 0;
		}
		if (p->vendor == 0x10ec && p->device == 0x8139) {	/* RTL8139: MAC only */
			u16 io = pci_bar_io(p, 0);
			if (!io) continue;
			pci_enable(p);
			for (int k = 0; k < 6; k++) sysinfo.mac[k] = inb(io + k);
			sysinfo.have_mac = true;
			snprintf(sysinfo.mac_dev, sizeof(sysinfo.mac_dev), "%s", p->path);
			return 0;
		}
	}
	return -1;
}

static int nic_init(void)
{
	if (nic_up) return 0;
	if (!nic) net_probe();
	if (!nic) { printf("No supported network interface (Intel e1000 family)\n"); return -1; }
	mmio = pci_bar_mem(nic, 0);
	memcpy(mymac, sysinfo.mac, 6);
	wr(0x00, rd(0x00) | (1u << 26));	/* reset */
	mdelay(20);
	wr(0xd8, 0xffffffff);			/* mask interrupts */
	wr(0x00, (rd(0x00) | (1 << 6) | (1 << 5)) & ~((1u << 3) | (1u << 31) | (1u << 7)));
	wr(0x5400, mymac[0] | (mymac[1] << 8) | (mymac[2] << 16) | ((u32)mymac[3] << 24));
	wr(0x5404, mymac[4] | (mymac[5] << 8) | 0x80000000u);
	for (int i = 0; i < 128; i++) wr(0x5200 + i * 4, 0);
	rx = memalign(128, NRX * sizeof(*rx));
	tx = memalign(128, NTX * sizeof(*tx));
	rxbuf = memalign(4096, NRX * BUFSZ);
	txbuf = memalign(4096, NTX * BUFSZ);
	if (!rx || !tx || !rxbuf || !txbuf) return -1;
	memset(rx, 0, NRX * sizeof(*rx));
	memset(tx, 0, NTX * sizeof(*tx));
	for (int i = 0; i < NRX; i++) rx[i].addr = (uintptr_t)(rxbuf + i * BUFSZ);
	wr(0x2800, (uintptr_t)rx); wr(0x2804, 0);
	wr(0x2808, NRX * sizeof(*rx));
	wr(0x2810, 0); wr(0x2818, NRX - 1);
	rx_tail = 0;
	wr(0x0100, (1 << 1) | (1 << 15) | (1 << 26) | (1 << 4) | (1 << 3));	/* EN BAM SECRC MPE UPE */
	wr(0x3800, (uintptr_t)tx); wr(0x3804, 0);
	wr(0x3808, NTX * sizeof(*tx));
	wr(0x3810, 0); wr(0x3818, 0);
	tx_tail = 0;
	wr(0x0410, 0x0060200a);
	wr(0x0400, (1 << 1) | (1 << 3) | (0x0f << 4) | (0x40 << 12));
	u64 end = timer_us() + 3000000;
	while (!(rd(0x08) & 2) && timer_us() < end) mdelay(10);	/* STATUS.LU */
	nic_up = true;
	return 0;
}

static void eth_send(const u8 *dst, u16 type, const void *payload, u32 len)
{
	u8 *b = txbuf + tx_tail * BUFSZ;
	memcpy(b, dst, 6);
	memcpy(b + 6, mymac, 6);
	b[12] = type >> 8; b[13] = type;
	memcpy(b + 14, payload, len);
	u32 tot = MAX(len + 14, 60u);
	if (tot > len + 14) memset(b + 14 + len, 0, tot - len - 14);
	struct txd *d = &tx[tx_tail];
	d->addr = (uintptr_t)b;
	d->len = tot;
	d->cmd = 1 | 2 | 8;
	d->status = 0;
	tx_tail = (tx_tail + 1) % NTX;
	mb();
	wr(0x3818, tx_tail);
	u64 end = timer_us() + 100000;
	while (!(d->status & 1) && timer_us() < end)
		;
}

/* returns frame length, 0 if none */
static u32 eth_recv(u8 *out)
{
	struct rxd *d = &rx[rx_tail];
	if (!(d->status & 1)) return 0;
	u32 len = d->len;
	if (len > BUFSZ) len = BUFSZ;
	memcpy(out, rxbuf + rx_tail * BUFSZ, len);
	d->status = 0;
	wr(0x2818, rx_tail);
	rx_tail = (rx_tail + 1) % NRX;
	return len;
}

/* ---------------- IP / UDP ---------------- */
static u16 csum(const void *p, u32 n, u32 sum)
{
	const u8 *b = p;
	for (u32 i = 0; i + 1 < n; i += 2) sum += (b[i] << 8) | b[i + 1];
	if (n & 1) sum += b[n - 1] << 8;
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

static u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static u8 arp_mac[6];
static u32 arp_ip;

static void put32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u32 get32(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static void arp_send(u16 op, const u8 *tmac, u32 tip)
{
	u8 a[28] = { 0, 1, 8, 0, 6, 4, 0, op };
	memcpy(a + 8, mymac, 6);
	put32(a + 14, myip);
	memcpy(a + 18, tmac, 6);
	put32(a + 24, tip);
	eth_send(op == 1 ? bcast : tmac, 0x0806, a, 28);
}

static void udp_send(u32 dip, const u8 *dmac, u16 sport, u16 dport, const void *data, u32 len)
{
	u8 pkt[1600];
	u8 *ip = pkt, *udp = pkt + 20;
	memset(pkt, 0, 28);
	ip[0] = 0x45; ip[2] = (len + 28) >> 8; ip[3] = len + 28;
	ip[6] = 0x40; ip[8] = 64; ip[9] = 17;
	put32(ip + 12, myip); put32(ip + 16, dip);
	u16 c = csum(ip, 20, 0); ip[10] = c >> 8; ip[11] = c;
	udp[0] = sport >> 8; udp[1] = sport; udp[2] = dport >> 8; udp[3] = dport;
	udp[4] = (len + 8) >> 8; udp[5] = len + 8;
	memcpy(udp + 8, data, len);
	eth_send(dmac, 0x0800, pkt, len + 28);
}

/* Process one frame; returns UDP payload length if it matches port, else -1. */
static int net_poll(u16 port, u8 *frame, u8 **payload, u32 *src_ip, u16 *src_port)
{
	u32 n = eth_recv(frame);
	if (!n) return -1;
	u16 type = (frame[12] << 8) | frame[13];
	u8 *p = frame + 14;
	if (type == 0x0806 && n >= 42) {
		u16 op = (p[6] << 8) | p[7];
		u32 sip = get32(p + 14), tip = get32(p + 24);
		if (sip == arp_ip) memcpy(arp_mac, p + 8, 6);
		if (op == 1 && myip && tip == myip) arp_send(2, p + 8, sip);
		return -1;
	}
	if (type != 0x0800 || (p[0] >> 4) != 4 || p[9] != 17) return -1;
	u32 ihl = (p[0] & 15) * 4;
	u8 *udp = p + ihl;
	u16 dport = (udp[2] << 8) | udp[3];
	if (dport != port) return -1;
	*src_ip = get32(p + 12);
	*src_port = (udp[0] << 8) | udp[1];
	*payload = udp + 8;
	return ((udp[4] << 8) | udp[5]) - 8;
}

static int arp_resolve(u32 ip, u8 *mac)
{
	u32 hop = ((ip & netmask) == (myip & netmask) || !gateway_ip) ? ip : gateway_ip;
	arp_ip = hop;
	memset(arp_mac, 0, 6);
	u8 frame[BUFSZ], *pl;
	u32 sip; u16 sp;
	for (int tries = 0; tries < 6; tries++) {
		arp_send(1, (u8[6]){ 0 }, hop);
		u64 end = timer_us() + 500000;
		while (timer_us() < end) {
			net_poll(0, frame, &pl, &sip, &sp);
			if (arp_mac[0] | arp_mac[1] | arp_mac[2] | arp_mac[3] | arp_mac[4] | arp_mac[5]) {
				memcpy(mac, arp_mac, 6);
				return 0;
			}
		}
	}
	return -1;
}

/* ---------------- DHCP ---------------- */
static char dhcp_file[128], dhcp_sname[64];
static u32 xid;

static int dhcp_exchange(void)
{
	u8 frame[BUFSZ], *pl;
	u32 sip; u16 sp;
	u32 offered = 0, sid = 0;
	xid = (u32)rdtsc();
	for (int phase = 0; phase < 2; phase++) {
		for (int tries = 0; tries < 4; tries++) {
			u8 m[300];
			memset(m, 0, sizeof(m));
			m[0] = 1; m[1] = 1; m[2] = 6;
			put32(m + 4, xid);
			m[10] = 0x80;	/* broadcast flag */
			memcpy(m + 28, mymac, 6);
			u8 *o = m + 236;
			put32(o, 0x63825363); o += 4;
			*o++ = 53; *o++ = 1; *o++ = phase ? 3 : 1;
			if (phase) {
				*o++ = 50; *o++ = 4; put32(o, offered); o += 4;
				*o++ = 54; *o++ = 4; put32(o, sid); o += 4;
			}
			*o++ = 55; *o++ = 6; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 66; *o++ = 67; *o++ = 15;
			*o++ = 60; *o++ = 9; memcpy(o, "PXEClient", 9); o += 9;
			*o++ = 255;
			u32 save = myip;
			myip = 0;
			udp_send(0xffffffff, bcast, 68, 67, m, sizeof(m));
			myip = save;
			u64 end = timer_us() + 2000000 * (tries + 1);
			while (timer_us() < end) {
				int n = net_poll(68, frame, &pl, &sip, &sp);
				if (n < 240 || pl[0] != 2 || get32(pl + 4) != xid) continue;
				u8 type = 0;
				u32 mask = 0, router = 0, s_id = 0;
				char bootfile[128] = "", tftpname[64] = "";
				for (u8 *q = pl + 240; q < pl + n && *q != 255;) {
					if (*q == 0) { q++; continue; }
					u8 code = q[0], l = q[1];
					u8 *v = q + 2;
					if (code == 53) type = v[0];
					else if (code == 1 && l == 4) mask = get32(v);
					else if (code == 3 && l >= 4) router = get32(v);
					else if (code == 54 && l == 4) s_id = get32(v);
					else if (code == 67) { memcpy(bootfile, v, MIN(l, 127)); bootfile[MIN(l, 127)] = 0; }
					else if (code == 66) { memcpy(tftpname, v, MIN(l, 63)); tftpname[MIN(l, 63)] = 0; }
					q += 2 + l;
				}
				if (phase == 0 && type == 2) {
					offered = get32(pl + 16);
					sid = s_id;
					break;
				}
				if (phase == 1 && type == 5) {
					myip = get32(pl + 16);
					netmask = mask ? mask : 0xffffff00;
					gateway_ip = router;
					server_ip = get32(pl + 20) ? get32(pl + 20) : s_id;
					if (bootfile[0]) strlcpy(dhcp_file, bootfile, sizeof(dhcp_file));
					else { memcpy(dhcp_file, pl + 108, 127); dhcp_file[127] = 0; }
					strlcpy(dhcp_sname, tftpname, sizeof(dhcp_sname));
					return 0;
				}
				if (phase == 1 && type == 6) return -1;	/* NAK */
			}
			if (phase == 0 && offered) break;
			if (phase == 1 && myip) break;
		}
		if (phase == 0 && !offered) return -1;
	}
	return myip ? 0 : -1;
}

/* ---------------- TFTP ---------------- */
static u8 server_mac[6];

static int tftp_get(const char *name, struct src *out, bool quiet)
{
	u8 frame[BUFSZ], *pl;
	u32 sip; u16 sp;
	u16 lport = 2000 + (rdtsc() & 0x3fff);
	u8 req[300];
	int n = 0;
	req[n++] = 0; req[n++] = 1;
	n += snprintf((char *)req + n, 200, "%s", name) + 1;
	n += snprintf((char *)req + n, 20, "octet") + 1;
	n += snprintf((char *)req + n, 20, "tsize") + 1;
	n += snprintf((char *)req + n, 20, "0") + 1;
	n += snprintf((char *)req + n, 20, "blksize") + 1;
	n += snprintf((char *)req + n, 20, "1432") + 1;
	udp_send(server_ip, server_mac, lport, 69, req, n);
	u32 blksize = 512, expect = 1, tsize = 0;
	u64 cap = 0, got = 0, buf = 0;
	u16 sport = 0;
	int retries = 0;
	u64 last = timer_us();
	static const char spin[] = "|/-\\";
	for (;;) {
		if (timer_us() - last > 3000000) {
			if (++retries > 5) { if (!quiet) printf("TFTP timeout on %s\n", name); return -1; }
			if (!sport) udp_send(server_ip, server_mac, lport, 69, req, n);
			else { u8 ack[4] = { 0, 4, (expect - 1) >> 8, expect - 1 }; udp_send(server_ip, server_mac, lport, sport, ack, 4); }
			last = timer_us();
		}
		int l = net_poll(lport, frame, &pl, &sip, &sp);
		if (l < 4 || sip != server_ip) continue;
		u16 op = (pl[0] << 8) | pl[1];
		if (op == 5) {
			if (!quiet) printf("TFTP error: %s (%s)\n", pl + 4, name);
			return -1;
		}
		if (!sport) sport = sp;
		if (sp != sport) continue;
		if (op == 6) {	/* OACK */
			for (u8 *q = pl + 2; q < pl + l;) {
				char *k = (char *)q, *v = k + strlen(k) + 1;
				if (!strcasecmp(k, "tsize")) tsize = strtoul(v, NULL, 10);
				if (!strcasecmp(k, "blksize")) blksize = strtoul(v, NULL, 10);
				q = (u8 *)v + strlen(v) + 1;
			}
			u8 ack[4] = { 0, 4, 0, 0 };
			udp_send(server_ip, server_mac, lport, sport, ack, 4);
			last = timer_us();
			continue;
		}
		if (op != 3) continue;
		u16 blk = (pl[2] << 8) | pl[3];
		u32 dl = l - 4;
		if (blk == (u16)expect) {
			if (!buf) {
				cap = tsize ? ALIGN_UP((u64)tsize, 4096) : 256ull * 1024 * 1024;
				buf = phys_alloc(cap, 4096, 0x100000, 0xfffff000ull, true);
				if (!buf) { printf("No memory for %s\n", name); return -1; }
			}
			if (got + dl > cap) { printf("%s is too large\n", name); return -1; }
			memcpy((void *)(uintptr_t)(buf + got), pl + 4, dl);
			got += dl;
			expect++;
			if (!quiet && (expect & 255) == 0) { console_putc(spin[(expect >> 8) & 3]); console_putc('\b'); console_flush(); }
		}
		u8 ack[4] = { 0, 4, blk >> 8, blk };
		udp_send(server_ip, server_mac, lport, sport, ack, 4);
		last = timer_us();
		retries = 0;
		if (blk == (u16)(expect - 1) && dl < blksize) break;
	}
	memset(out, 0, sizeof(*out));
	out->mem = (const u8 *)(uintptr_t)buf;
	out->size = got;
	strlcpy(out->name, name, sizeof(out->name));
	return 0;
}

static int net_open(void *ctx, const char *path, struct src *s)
{
	(void)ctx;
	while (*path == '/' && path[1] == '/') path++;
	return tftp_get(path, s, false);
}

static void ipstr(char *b, u32 ip) { snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }

int net_boot(const char *file, const char *args)
{
	if (nic_init() < 0) return -1;
	phys_reset();
	printf("Requesting Internet Address for %x:%x:%x:%x:%x:%x\n", mymac[0], mymac[1], mymac[2], mymac[3], mymac[4], mymac[5]);
	if (dhcp_exchange() < 0) { printf("No DHCP/BOOTP reply\n"); return -1; }
	char a[16], s[16];
	ipstr(a, myip); ipstr(s, server_ip);
	printf("Internet address is %s, boot server %s\n", a, s);
	if (arp_resolve(server_ip, server_mac) < 0) { printf("Can't resolve boot server\n"); return -1; }
	const char *name = (file && *file) ? file : dhcp_file;
	char dir[128] = "";
	strlcpy(dir, name, sizeof(dir));
	char *sl = strrchr(dir, '/');
	if (sl) sl[1] = 0; else dir[0] = 0;

	/* GRUB (Solaris pxegrub) and pxelinux menus */
	const char *base = sl ? name + (sl - dir) + 1 : name;
	char cfgname[192];
	struct src cfg;
	bool have_cfg = false;
	int kind = 0;
	if (!*name || strstr(base, "grub")) {
		snprintf(cfgname, sizeof(cfgname), "%smenu.lst.01%02X%02X%02X%02X%02X%02X", dir,
			 mymac[0], mymac[1], mymac[2], mymac[3], mymac[4], mymac[5]);
		have_cfg = tftp_get(cfgname, &cfg, true) == 0;
		if (!have_cfg) {
			snprintf(cfgname, sizeof(cfgname), "%sboot/grub/menu.lst", dir);
			have_cfg = tftp_get(cfgname, &cfg, true) == 0;
		}
		kind = 1;
	} else if (strstr(base, "pxelinux") || strstr(base, "lpxelinux")) {
		snprintf(cfgname, sizeof(cfgname), "%spxelinux.cfg/01-%02x-%02x-%02x-%02x-%02x-%02x", dir,
			 mymac[0], mymac[1], mymac[2], mymac[3], mymac[4], mymac[5]);
		have_cfg = tftp_get(cfgname, &cfg, true) == 0;
		if (!have_cfg) {
			snprintf(cfgname, sizeof(cfgname), "%spxelinux.cfg/%08X", dir, myip);
			have_cfg = tftp_get(cfgname, &cfg, true) == 0;
		}
		if (!have_cfg) {
			snprintf(cfgname, sizeof(cfgname), "%spxelinux.cfg/default", dir);
			have_cfg = tftp_get(cfgname, &cfg, true) == 0;
		}
		kind = 2;
	}
	if (have_cfg) {
		printf("Using %s\n", cfgname);
		char *text = malloc(cfg.size + 1);
		if (!text) return -1;
		memcpy(text, cfg.mem, cfg.size);
		text[cfg.size] = 0;
		char d[128];
		strlcpy(d, dir, sizeof(d));
		if (d[0] && d[strlen(d) - 1] == '/') d[strlen(d) - 1] = 0;
		return boot_config_text(text, kind, d, args, net_open, NULL);
	}
	if (!*name) { printf("No boot file name from DHCP\n"); return -1; }
	struct src k;
	if (tftp_get(name, &k, false) < 0) return -1;
	char cmd[512];
	u8 sig[4] = { 0 };
	src_read(&k, 0x202, 4, sig);
	if (!memcmp(sig, "HdrS", 4)) snprintf(cmd, sizeof(cmd), "%s", args ? args : "");
	else snprintf(cmd, sizeof(cmd), "%s%s%s", name, (args && *args) ? " " : "", args ? args : "");
	return boot_load_image(&k, cmd, NULL, NULL, 0, NULL, 0);
}
