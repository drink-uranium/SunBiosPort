/* Console multiplexer, 16550 UART, i8042 keyboard and line editor. */
#include "console.h"

/* ---------------- serial ---------------- */
static u16 ser_port = 0x3f8;
static uintptr_t ser_mmio;
static u32 ser_regw = 1;
static bool ser_ok;

static u8 ser_in(int reg)
{
	if (ser_mmio)
		return ser_regw == 4 ? (u8)read32(ser_mmio + reg * 4) : read8(ser_mmio + reg);
	return inb(ser_port + reg);
}

static void ser_out(int reg, u8 v)
{
	if (ser_mmio) {
		if (ser_regw == 4) write32(ser_mmio + reg * 4, v);
		else write8(ser_mmio + reg, v);
	} else {
		outb(ser_port + reg, v);
	}
}

void serial_init(u16 port, u32 baud, uintptr_t mmio, u32 regwidth)
{
	ser_port = port;
	ser_mmio = mmio;
	ser_regw = regwidth ? regwidth : 1;
	/* Scratch register probe: an absent UART floats to 0xff. */
	ser_out(7, 0x5a);
	if (ser_in(7) != 0x5a) { ser_ok = false; return; }
	ser_out(7, 0xa5);
	if (ser_in(7) != 0xa5) { ser_ok = false; return; }
	ser_ok = true;
	if (!baud) baud = 115200;
	u16 div = 115200 / baud;
	ser_out(1, 0x00);
	ser_out(3, 0x80);
	ser_out(0, div & 0xff);
	ser_out(1, div >> 8);
	ser_out(3, 0x03);
	ser_out(2, 0xc7);
	ser_out(4, 0x03);
}

bool serial_present(void) { return ser_ok; }

void serial_putc(char c)
{
	if (!ser_ok) return;
	for (int i = 0; i < 100000 && !(ser_in(5) & 0x20); i++)
		;
	ser_out(0, c);
}

int serial_getc_nb(void)
{
	if (!ser_ok || !(ser_in(5) & 1)) return -1;
	return ser_in(0);
}

/* ---------------- i8042 keyboard (scan code set 1, translated) ---------------- */
static bool kbd_ok;
static int shift, ctrl, caps, e0;

static const char kmap[128] = {
	0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's',
	'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
	'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
};
static const char kmap_shift[128] = {
	0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0, 'A', 'S',
	'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
	'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
};

static int kbd_wait_write(void)
{
	for (int i = 0; i < 100000; i++)
		if (!(inb(0x64) & 2)) return 0;
	return -1;
}

static int kbd_wait_read(void)
{
	for (int i = 0; i < 100000; i++)
		if (inb(0x64) & 1) return 0;
	return -1;
}

void kbd_init(void)
{
	u8 st = inb(0x64);
	kbd_ok = st != 0xff;
	if (!kbd_ok) return;
	/* Drain, then make sure the keyboard port is enabled and the controller
	 * translates to scan code set 1. A BIOS does this; coreboot leaves the
	 * keyboard in raw set 2. */
	for (int i = 0; i < 32 && (inb(0x64) & 1); i++) inb(0x60);
	if (kbd_wait_write() < 0) return;
	outb(0x64, 0x20);
	if (kbd_wait_read() < 0) return;
	u8 cb = inb(0x60);
	cb |= 0x40;		/* XLAT */
	cb &= ~0x10;		/* keyboard clock enabled */
	if (kbd_wait_write() < 0) return;
	outb(0x64, 0x60);
	if (kbd_wait_write() < 0) return;
	outb(0x60, cb);
	if (kbd_wait_write() == 0) outb(0x64, 0xae);
	for (int i = 0; i < 32 && (inb(0x64) & 1); i++) inb(0x60);
}

bool kbd_present(void) { return kbd_ok; }

int kbd_getc_nb(void)
{
	if (!kbd_ok) return -1;
	u8 st = inb(0x64);
	if (!(st & 1)) return -1;
	u8 sc = inb(0x60);
	if (st & 0x20) return -1;	/* mouse byte */
	if (sc == 0xe0) { e0 = 1; return -1; }
	int rel = sc & 0x80;
	sc &= 0x7f;
	if (e0) {
		e0 = 0;
		if (sc == 0x1d) { ctrl = !rel; return -1; }
		if (rel) return -1;
		switch (sc) {
		case 0x48: return KEY_UP;
		case 0x50: return KEY_DOWN;
		case 0x4b: return KEY_LEFT;
		case 0x4d: return KEY_RIGHT;
		case 0x47: return KEY_HOME;
		case 0x4f: return KEY_END;
		case 0x53: return KEY_DEL;
		case 0x1c: return '\r';
		case 0x35: return '/';
		}
		return -1;
	}
	switch (sc) {
	case 0x2a: case 0x36: shift = !rel; return -1;
	case 0x1d: ctrl = !rel; return -1;
	case 0x3a: if (!rel) caps = !caps; return -1;
	}
	if (rel) return -1;
	char c = shift ? kmap_shift[sc] : kmap[sc];
	if (!c) return -1;
	if (caps && isalpha(c)) c ^= 0x20;
	if (ctrl && isalpha(c)) c = tolower(c) - 'a' + 1;
	return (u8)c;
}

/* ---------------- multiplexer ---------------- */
static bool out_screen = true, out_serial = true;
static int col;
static int esc_state;	/* serial ANSI escape decoding */

void console_set_output(bool screen, bool serial) { out_screen = screen; out_serial = serial; }
void console_get_output(bool *screen, bool *serial) { *screen = out_screen; *serial = out_serial; }

void console_putc(char c)
{
	if (c == '\n') col = 0;
	else if (c == '\r') col = 0;
	else if (c == '\b') { if (col) col--; }
	else if (c == '\t') col = (col + 8) & ~7;
	else if ((u8)c >= 32) col++;
	if (out_serial) {
		if (c == '\n') serial_putc('\r');
		serial_putc(c);
	}
	if (out_screen && fb_active()) fb_putc(c);
}

void console_write(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) console_putc(s[i]);
	console_flush();
}

void console_flush(void) { if (fb_active()) fb_flush(); }
int console_column(void) { return col; }

int console_getc_nb(void)
{
	int c = kbd_getc_nb();
	if (c >= 0) return c;
	c = serial_getc_nb();
	if (c < 0) return -1;
	/* Translate VT100 cursor keys arriving over the serial line. */
	if (esc_state == 0 && c == 27) {
		u64 t = timer_us();
		while (timer_us() - t < 20000) {
			int d = serial_getc_nb();
			if (d < 0) continue;
			if (d != '[' && d != 'O') return d;
			while (timer_us() - t < 40000) {
				int e = serial_getc_nb();
				if (e < 0) continue;
				switch (e) {
				case 'A': return KEY_UP;
				case 'B': return KEY_DOWN;
				case 'C': return KEY_RIGHT;
				case 'D': return KEY_LEFT;
				case 'H': return KEY_HOME;
				case 'F': return KEY_END;
				case '3': serial_getc_nb(); return KEY_DEL;
				}
				return -1;
			}
			return 27;
		}
		return 27;
	}
	if (c == 0x7f) c = 8;
	return c;
}

int console_getc(void)
{
	int c;
	fb_cursor(true);
	console_flush();
	while ((c = console_getc_nb()) < 0)
		__asm__ volatile("pause");
	fb_cursor(false);
	return c;
}

/* ---------------- line editor with history ---------------- */
#define HIST 16
static char history[HIST][256];
static int hist_n;

static void redraw_tail(const char *buf, int len, int pos, int erase)
{
	for (int i = pos; i < len; i++) console_putc(buf[i]);
	for (int i = 0; i < erase; i++) console_putc(' ');
	for (int i = 0; i < len - pos + erase; i++) console_putc('\b');
}

int readline(const char *prompt, char *buf, int max)
{
	int len = 0, pos = 0, h = hist_n;
	if (prompt) puts_raw(prompt);
	buf[0] = 0;
	for (;;) {
		console_flush();
		int c = console_getc();
		if (c == '\r' || c == '\n') {
			buf[len] = 0;
			console_putc('\n');
			console_flush();
			if (len && (hist_n == 0 || strcmp(history[(hist_n - 1) % HIST], buf))) {
				strlcpy(history[hist_n % HIST], buf, sizeof(history[0]));
				hist_n++;
			}
			return len;
		}
		if (c == 8) {
			if (!pos) continue;
			memmove(buf + pos - 1, buf + pos, len - pos);
			pos--; len--;
			console_putc('\b');
			redraw_tail(buf, len, pos, 1);
		} else if (c == KEY_DEL) {
			if (pos == len) continue;
			memmove(buf + pos, buf + pos + 1, len - pos - 1);
			len--;
			redraw_tail(buf, len, pos, 1);
		} else if (c == KEY_LEFT) {
			if (pos) { pos--; console_putc('\b'); }
		} else if (c == KEY_RIGHT) {
			if (pos < len) console_putc(buf[pos++]);
		} else if (c == KEY_HOME) {
			while (pos) { pos--; console_putc('\b'); }
		} else if (c == KEY_END) {
			while (pos < len) console_putc(buf[pos++]);
		} else if (c == 0x15 || c == KEY_UP || c == KEY_DOWN) {	/* ^U, history */
			if (c == KEY_UP) {
				if (h == 0 || h <= hist_n - HIST) continue;
				h--;
			} else if (c == KEY_DOWN) {
				if (h >= hist_n) continue;
				h++;
			}
			while (pos < len) console_putc(buf[pos++]);
			int old = len;
			while (pos) { pos--; console_putc('\b'); }
			if (c == 0x15 || h == hist_n) len = 0;
			else len = strlcpy(buf, history[h % HIST], max) < (size_t)max ? (int)strlen(buf) : max - 1;
			pos = len;
			redraw_tail(buf, len, 0, old > len ? old - len : 0);
			for (int i = 0; i < len; i++) console_putc(buf[i]);
		} else if (c == 0x03) {	/* ^C */
			buf[0] = 0;
			puts_raw("\n");
			return 0;
		} else if (c >= 32 && c < 127 && len < max - 1) {
			memmove(buf + pos + 1, buf + pos, len - pos);
			buf[pos] = c;
			len++;
			console_putc(c);
			pos++;
			redraw_tail(buf, len, pos, 0);
		}
	}
}
