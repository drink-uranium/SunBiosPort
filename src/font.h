#ifndef FONT_H
#define FONT_H

/* Layout of an OpenBoot "romfont" (see obp/pkg/termemu/font.fth). Glyphs
 * store height-1 scan lines; the last (blank) line overlaps the next glyph. */
struct obp_font {
	unsigned short width, height, advance, min_char, nglyphs;
	unsigned char bits[];
};

extern const struct obp_font gallant_font;
extern const unsigned char sun_logo[64 * 8];

#endif
