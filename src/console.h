#ifndef CONSOLE_H
#define CONSOLE_H

#include "lib.h"

/* Key codes beyond ASCII are mapped onto the emacs-style control keys that
 * the OpenBoot command line editor uses. */
#define KEY_UP    0x10	/* ^P */
#define KEY_DOWN  0x0e	/* ^N */
#define KEY_LEFT  0x02	/* ^B */
#define KEY_RIGHT 0x06	/* ^F */
#define KEY_HOME  0x01	/* ^A */
#define KEY_END   0x05	/* ^E */
#define KEY_DEL   0x04	/* ^D */
#define KEY_STOPA 0x1000	/* F1-A (Solaris x86's Stop-A) or a serial BREAK */

struct fb_info {
	uintptr_t base;
	u32 width, height, pitch, bpp;
	u8 rpos, rsize, gpos, gsize, bpos, bsize;
	int text_mode;	/* legacy VGA 80x25 text at 0xB8000 */
};

void serial_init(u16 port, u32 baud, uintptr_t mmio, u32 regwidth);
void serial_putc(char c);
int serial_getc_nb(void);
bool serial_present(void);

void kbd_init(void);
int kbd_getc_nb(void);
bool kbd_present(void);

void fb_init(const struct fb_info *fb);
bool fb_active(void);
bool fb_is_graphics(void);
void fb_putc(char c);
void fb_flush(void);
void fb_draw_logo(int line);
int fb_line(void);
int fb_column(void);
int fb_rows(void);
int fb_cols(void);
void fb_set_line(int line);
void fb_clear(void);
void fb_cursor(bool on);
const struct fb_info *fb_get_info(void);
void fb_shutdown(void);
u32 fb_logo_color(void);
void fb_set_colors(u32 fg, u32 bg);

void console_write(const char *s, size_t n);
void console_putc(char c);
void console_flush(void);
int console_getc_nb(void);
int console_getc(void);
int console_column(void);
void console_set_output(bool screen, bool serial);
void console_get_output(bool *screen, bool *serial);

int readline(const char *prompt, char *buf, int len);

#endif
