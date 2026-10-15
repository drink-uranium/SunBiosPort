typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8; typedef unsigned long long u64;
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static int row, col;
static void sputc(char c) { if (c == '\n') sputc('\r'); while (!(inb(0x3fd) & 0x20)); outb(0x3f8, c); }
static void puts_(const char *s) {
	volatile u16 *v = (u16 *)0xb8000;

	for (const char *p = s; *p; p++) {
		sputc(*p);
		if (*p == 10) { row++; col = 0; continue; }
		if (row < 25 && col < 80) v[row * 80 + col++] = 0x1f00 | (u8)*p;
	}
}
static void hex(u32 v) { char b[11] = "0x"; for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15]; b[10] = 0; puts_(b); }
static void dec(u32 v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); puts_(b + i); }
static void str_n(const char *s, u32 n) { char b[80]; u32 i; for (i = 0; i < n && i < 79 && s[i]; i++) b[i] = s[i]; b[i] = 0; puts_(b); }
static u32 sum(const u8 *p, u32 n) { u32 s = 0; for (u32 i = 0; i < n; i++) s = s * 31 + p[i]; return s; }

void kmain(u32 magic, u32 *mbi)
{
	puts_("MBTEST: kernel entered, magic "); hex(magic); puts_("\n");
	if (magic == 0x2badb002) {
		u32 f = mbi[0];
		puts_("MBTEST: flags "); hex(f); puts_(" mem_lower "); dec(mbi[1]); puts_("K mem_upper "); dec(mbi[2]); puts_("K\n");
		if (f & 4) { puts_("MBTEST: cmdline '"); puts_((char *)mbi[4]); puts_("'\n"); }
		if (f & 512) { puts_("MBTEST: loader '"); puts_((char *)mbi[16]); puts_("'\n"); }
		if (f & 8) {
			u32 *m = (u32 *)mbi[6];
			for (u32 i = 0; i < mbi[5]; i++, m += 4) {
				puts_("MBTEST: module "); dec(i); puts_(" at "); hex(m[0]); puts_(" size "); dec(m[1] - m[0]);
				puts_(" sum "); hex(sum((u8 *)m[0], m[1] - m[0])); puts_(" '"); puts_((char *)m[2]); puts_("' data '");
				str_n((char *)m[0], 24); puts_("'\n");
			}
		}
		if (f & 64) { puts_("MBTEST: mmap entries "); dec(mbi[11] / 24); puts_("\n"); }
		if (f & 4096) { puts_("MBTEST: framebuffer "); hex(mbi[22]); puts_("\n"); }
	} else if (magic == 0x36d76289) {
		u8 *p = (u8 *)mbi + 8, *end = (u8 *)mbi + mbi[0];
		while (p < end) {
			u32 type = *(u32 *)p, size = *(u32 *)(p + 4);
			if (type == 0) break;
			if (type == 1) { puts_("MBTEST: mb2 cmdline '"); puts_((char *)p + 8); puts_("'\n"); }
			if (type == 2) { puts_("MBTEST: mb2 loader '"); puts_((char *)p + 8); puts_("'\n"); }
			if (type == 3) { u32 *m = (u32 *)(p + 8); puts_("MBTEST: mb2 module size "); dec(m[1] - m[0]); puts_(" sum "); hex(sum((u8 *)m[0], m[1] - m[0])); puts_(" '"); puts_((char *)p + 16); puts_("'\n"); }
			if (type == 4) { puts_("MBTEST: mb2 mem_upper "); dec(*(u32 *)(p + 12)); puts_("K\n"); }
			if (type == 6) { puts_("MBTEST: mb2 mmap\n"); }
			if (type == 8) { puts_("MBTEST: mb2 framebuffer type "); dec(p[29]); puts_("\n"); }
			if (type == 14 || type == 15) { puts_("MBTEST: mb2 acpi rsdp\n"); }
			p += (size + 7) & ~7u;
		}
	}
	puts_("MBTEST: done\n");
	outb(0xf4, 0); /* isa-debug-exit, if present */
}
