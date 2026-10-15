/*
 * OpenBoot-style framebuffer terminal emulator.
 *
 * Mirrors obp/pkg/termemu: an 80x34 character window of 12x22 Gallant glyphs
 * centred on the screen, black text on a white background, and the 64x64
 * Sun logo drawn in the left margin next to the banner. Everything is drawn
 * into a 32bpp shadow buffer and flushed to the real framebuffer, so
 * scrolling never reads from (slow) video memory.
 */
#include "console.h"
#include "font.h"

#define COLS_MAX 80
#define ROWS_MAX 34

static struct fb_info fb;
static bool active, gfx;
static u32 *shadow;		/* window-sized, 32bpp 0x00RRGGBB */
static int win_x, win_y, win_w, win_h;
static int cols, rows, cw, ch;
static int cx, cy;
static bool cursor_on;
static int dirty_y0 = 1 << 30, dirty_y1 = -1;
static u32 fg = 0x000000, bg = 0xffffff, logo_rgb = 0x7700b1;
static bool inverse;
/* ANSI (x3.64) escape parsing, as in obp/pkg/termemu/ansiterm.fth */
static int esc, nparam, params[4];

/* text-mode fallback */
static u16 *vga_text = (u16 *)0xb8000;

const struct fb_info *fb_get_info(void) { return &fb; }
bool fb_active(void) { return active; }
bool fb_is_graphics(void) { return active && gfx; }
int fb_line(void) { return cy; }
int fb_column(void) { return cx; }
int fb_rows(void) { return rows; }
int fb_cols(void) { return cols; }
u32 fb_logo_color(void) { return logo_rgb; }

static void mark(int y0, int y1)
{
	if (y0 < dirty_y0) dirty_y0 = y0;
	if (y1 > dirty_y1) dirty_y1 = y1;
}

static inline u32 conv(u32 rgb)
{
	u32 r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	return ((r >> (8 - fb.rsize)) << fb.rpos) | ((g >> (8 - fb.gsize)) << fb.gpos) |
	       ((b >> (8 - fb.bsize)) << fb.bpos);
}

void fb_flush(void)
{
	if (!active || !gfx || dirty_y1 < dirty_y0) return;
	bool native32 = fb.bpp == 32 && fb.rpos == 16 && fb.gpos == 8 && fb.bpos == 0 &&
			fb.rsize == 8 && fb.gsize == 8 && fb.bsize == 8;
	for (int y = dirty_y0; y <= dirty_y1 && y < win_h; y++) {
		u32 *src = shadow + y * win_w;
		uintptr_t dst = fb.base + (uintptr_t)(win_y + y) * fb.pitch + (uintptr_t)win_x * (fb.bpp / 8);
		if (native32) {
			memcpy((void *)dst, src, win_w * 4);
			continue;
		}
		for (int x = 0; x < win_w; x++) {
			u32 v = conv(src[x]);
			switch (fb.bpp) {
			case 32: ((volatile u32 *)dst)[x] = v; break;
			case 24:
				((volatile u8 *)dst)[x * 3] = v;
				((volatile u8 *)dst)[x * 3 + 1] = v >> 8;
				((volatile u8 *)dst)[x * 3 + 2] = v >> 16;
				break;
			case 16: case 15: ((volatile u16 *)dst)[x] = v; break;
			}
		}
	}
	dirty_y0 = 1 << 30;
	dirty_y1 = -1;
}

static void fill_rect(int x, int y, int w, int h, u32 c)
{
	for (int j = y; j < y + h; j++) {
		u32 *p = shadow + j * win_w + x;
		for (int i = 0; i < w; i++) p[i] = c;
	}
	mark(y, y + h - 1);
}

static void clear_screen_full(void)
{
	u32 v = conv(bg);
	for (u32 y = 0; y < fb.height; y++) {
		uintptr_t row = fb.base + (uintptr_t)y * fb.pitch;
		for (u32 x = 0; x < fb.width; x++) {
			switch (fb.bpp) {
			case 32: ((volatile u32 *)row)[x] = v; break;
			case 24:
				((volatile u8 *)row)[x * 3] = v;
				((volatile u8 *)row)[x * 3 + 1] = v >> 8;
				((volatile u8 *)row)[x * 3 + 2] = v >> 16;
				break;
			default: ((volatile u16 *)row)[x] = v; break;
			}
		}
	}
}

static void draw_glyph(int col, int row, u8 c, bool inv)
{
	if (!gfx) {
		vga_text[row * 80 + col] = (inv ? 0x7000 : 0x0700) | c;
		return;
	}
	const struct obp_font *f = &gallant_font;
	u32 f_c = inv ? bg : fg, b_c = inv ? fg : bg;
	if (inverse) { u32 t = f_c; f_c = b_c; b_c = t; }
	int gi = c - f->min_char;
	if (gi < 0 || gi >= f->nglyphs) gi = '?' - f->min_char;
	const u8 *g = f->bits + gi * (f->height - 1) * f->advance;
	int px = col * cw, py = row * ch;
	for (int y = 0; y < ch; y++) {
		u32 bits = 0;
		if (y < f->height - 1 && (g + y * 2 + 1) < f->bits + f->nglyphs * (f->height - 1) * f->advance)
			bits = (g[y * 2] << 8) | g[y * 2 + 1];
		u32 *p = shadow + (py + y) * win_w + px;
		for (int x = 0; x < cw; x++)
			p[x] = (bits & (0x8000 >> x)) ? f_c : b_c;
	}
	mark(py, py + ch - 1);
}

/* Character cells, kept so the cursor can be toggled by redrawing. */
static u8 cells[ROWS_MAX][COLS_MAX];

static void put_cell(int x, int y, u8 c)
{
	cells[y][x] = c;
	draw_glyph(x, y, c, false);
}

static void show_cursor(bool on)
{
	if (cx >= cols) return;
	if (!gfx) {
		u16 pos = cy * 80 + cx;
		outb(0x3d4, 0x0f); outb(0x3d5, pos & 0xff);
		outb(0x3d4, 0x0e); outb(0x3d5, pos >> 8);
		return;
	}
	u8 c = cells[cy][cx];
	draw_glyph(cx, cy, c ? c : ' ', on);
}

void fb_cursor(bool on)
{
	if (!active || on == cursor_on) return;
	cursor_on = on;
	show_cursor(on);
	if (on) fb_flush();
}

static void scroll(void)
{
	if (!gfx) {
		memmove(vga_text, vga_text + 80, 80 * 24 * 2);
		for (int i = 0; i < 80; i++) vga_text[24 * 80 + i] = 0x0720;
	} else {
		memmove(shadow, shadow + ch * win_w, (size_t)(win_h - ch) * win_w * 4);
		fill_rect(0, win_h - ch, win_w, ch, bg);
		mark(0, win_h - 1);
	}
	memmove(cells[0], cells[1], (ROWS_MAX - 1) * COLS_MAX);
	memset(cells[rows - 1], 0, COLS_MAX);
}

void fb_clear(void)
{
	if (!active) return;
	if (gfx) fill_rect(0, 0, win_w, win_h, bg);
	else for (int i = 0; i < 80 * 25; i++) vga_text[i] = 0x0720;
	memset(cells, 0, sizeof(cells));
	cx = cy = 0;
}

void fb_set_line(int line)
{
	if (line < 0) line = 0;
	if (line >= rows) line = rows - 1;
	cy = line;
}

void fb_set_colors(u32 f, u32 b) { fg = f; bg = b; }

static void newline(void)
{
	cx = 0;
	if (++cy >= rows) { scroll(); cy = rows - 1; }
}

void fb_putc(char ch_)
{
	u8 c = (u8)ch_;
	if (!active) return;
	if (cursor_on) { show_cursor(false); cursor_on = false; }
	if (esc == 1) {
		if (c == '[') { esc = 2; nparam = 0; params[0] = params[1] = 0; return; }
		esc = 0;
		return;
	}
	if (esc == 2) {
		if (isdigit(c)) {
			if (nparam == 0) nparam = 1;
			params[nparam - 1] = params[nparam - 1] * 10 + (c - '0');
			return;
		}
		if (c == ';') {
			if (nparam < 4) { if (nparam == 0) nparam = 1; nparam++; params[nparam - 1] = 0; }
			return;
		}
		esc = 0;
		int p0 = nparam ? params[0] : 0, n = p0 ? p0 : 1;
		switch (c) {
		case 'A': cy = MAX(0, cy - n); break;
		case 'B': cy = MIN(rows - 1, cy + n); break;
		case 'C': cx = MIN(cols - 1, cx + n); break;
		case 'D': cx = MAX(0, cx - n); break;
		case 'H': case 'f':
			cy = MIN(rows - 1, MAX(1, nparam ? params[0] : 1) - 1);
			cx = MIN(cols - 1, MAX(1, nparam > 1 ? params[1] : 1) - 1);
			break;
		case 'J':
			for (int y = cy; y < rows; y++)
				for (int x = (y == cy ? cx : 0); x < cols; x++) put_cell(x, y, ' ');
			break;
		case 'K':
			for (int x = cx; x < cols; x++) put_cell(x, cy, ' ');
			break;
		case 'm':
			inverse = p0 == 7;
			break;
		}
		return;
	}
	switch (c) {
	case 27: esc = 1; return;
	case '\n': newline(); return;
	case '\r': cx = 0; return;
	case '\b': if (cx) cx--; return;
	case '\t': do { fb_putc(' '); } while (cx & 7); return;
	case '\f': fb_clear(); return;
	case 7: return;
	}
	if (c < 32) return;
	if (cx >= cols) newline();
	put_cell(cx, cy, c);
	cx++;
}

void fb_draw_logo(int line)
{
	if (!active || !gfx) return;
	int py = line * ch;
	if (py + 64 > win_h) py = win_h - 64;
	for (int y = 0; y < 64; y++) {
		u32 *p = shadow + (py + y) * win_w;
		for (int x = 0; x < 64; x++)
			if (sun_logo[y * 8 + x / 8] & (0x80 >> (x & 7)))
				p[x] = logo_rgb;
	}
	mark(py, py + 63);
	fb_flush();
}

void fb_init(const struct fb_info *info)
{
	fb = *info;
	if (fb.text_mode) {
		gfx = false;
		cols = 80; rows = 25;
		active = true;
		fb_clear();
		return;
	}
	if (fb.bpp != 32 && fb.bpp != 24 && fb.bpp != 16 && fb.bpp != 15) return;
	cw = gallant_font.width;
	ch = gallant_font.height;
	cols = MIN(COLS_MAX, (int)fb.width / cw);
	rows = MIN(ROWS_MAX, (int)fb.height / ch);
	win_w = cols * cw;
	win_h = rows * ch;
	win_x = (fb.width - win_w) / 2;
	win_y = (fb.height - win_h) / 2;
	shadow = malloc((size_t)win_w * win_h * 4);
	if (!shadow) return;
	gfx = true;
	active = true;
	clear_screen_full();
	fb_clear();
	fb_flush();
}

void fb_shutdown(void)
{
	active = false;
}
