/* Minimal freestanding C library for the firmware. */
#include "lib.h"
#include "console.h"

void *memset(void *d, int c, size_t n)
{
	void *ret = d;
	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return ret;
}

void *memcpy(void *d, const void *s, size_t n)
{
	void *ret = d;
	size_t w = n >> 2;
	__asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(w) : : "memory");
	n &= 3;
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return ret;
}

void *memmove(void *d, const void *s, size_t n)
{
	if ((uintptr_t)d <= (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n)
		return memcpy(d, s, n);
	u8 *dp = (u8 *)d + n - 1;
	const u8 *sp = (const u8 *)s + n - 1;
	__asm__ volatile("std; rep movsb; cld" : "+D"(dp), "+S"(sp), "+c"(n) : : "memory");
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const u8 *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b) return (u8)*a - (u8)*b;
		if (!*a) return 0;
	}
	return 0;
}

int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isxdigit(int c) { return isdigit(c) || (tolower(c) >= 'a' && tolower(c) <= 'f'); }
int isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
int isprint(int c) { return c >= 32 && c < 127; }

int strcasecmp(const char *a, const char *b)
{
	while (*a && tolower(*a) == tolower(*b)) a++, b++;
	return tolower((u8)*a) - tolower((u8)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (tolower(*a) != tolower(*b)) return tolower((u8)*a) - tolower((u8)*b);
		if (!*a) return 0;
	}
	return 0;
}

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++) d[i] = s[i];
	for (; i < n; i++) d[i] = 0;
	return d;
}
size_t strlcpy(char *d, const char *s, size_t n)
{
	size_t l = strlen(s);
	if (n) {
		size_t c = l >= n ? n - 1 : l;
		memcpy(d, s, c);
		d[c] = 0;
	}
	return l;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c)
{
	for (; *s; s++) if (*s == (char)c) return (char *)s;
	return c ? NULL : (char *)s;
}
char *strrchr(const char *s, int c)
{
	const char *r = NULL;
	for (; *s; s++) if (*s == (char)c) r = s;
	return (char *)(c ? r : s);
}
char *strstr(const char *h, const char *n)
{
	size_t l = strlen(n);
	for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
	return l ? NULL : (char *)h;
}
char *strdup(const char *s)
{
	char *d = malloc(strlen(s) + 1);
	if (d) strcpy(d, s);
	return d;
}

unsigned long strtoul(const char *s, char **end, int base)
{
	unsigned long v = 0;
	while (isspace(*s)) s++;
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2, base = 16;
	if (base == 0) base = 10;
	for (;; s++) {
		int d;
		if (isdigit(*s)) d = *s - '0';
		else if (isalpha(*s)) d = tolower(*s) - 'a' + 10;
		else break;
		if (d >= base) break;
		v = v * base + d;
	}
	if (end) *end = (char *)s;
	return v;
}

/* ---- printf ---- */
struct outbuf { char *buf; size_t n, pos; };

static void ob_put(struct outbuf *o, char c)
{
	if (o->pos + 1 < o->n) o->buf[o->pos] = c;
	o->pos++;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
	struct outbuf o = { buf, n, 0 };
	for (; *fmt; fmt++) {
		if (*fmt != '%') { ob_put(&o, *fmt); continue; }
		fmt++;
		int left = 0, zero = 0, width = 0, prec = -1, lng = 0, alt = 0;
		for (;; fmt++) {
			if (*fmt == '-') left = 1;
			else if (*fmt == '0') zero = 1;
			else if (*fmt == '#') alt = 1;
			else break;
		}
		if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
		while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			fmt++; prec = 0;
			if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
			while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
		}
		while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') { if (*fmt == 'l') lng++; fmt++; }
		char tmp[24];
		const char *s = tmp;
		int len = 0, neg = 0;
		u64 v = 0;
		int base = 10;
		switch (*fmt) {
		case 'c': tmp[0] = (char)va_arg(ap, int); len = 1; break;
		case 's':
			s = va_arg(ap, const char *);
			if (!s) s = "(null)";
			len = prec >= 0 ? (int)strnlen(s, prec) : (int)strlen(s);
			break;
		case 'd': case 'i': {
			s64 x = lng >= 2 ? va_arg(ap, s64) : va_arg(ap, int);
			if (x < 0) { neg = 1; v = -x; } else v = x;
			goto num;
		}
		case 'p': alt = 1; lng = 0; base = 16; v = (uintptr_t)va_arg(ap, void *); goto num_base;
		case 'x': case 'X': base = 16; goto unum;
		case 'o': base = 8; goto unum;
		case 'u':
		unum:
			v = lng >= 2 ? va_arg(ap, u64) : va_arg(ap, unsigned int);
		num_base:
		num: {
			char *p = tmp + sizeof(tmp);
			const char *digits = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
			do { *--p = digits[v % base]; v /= base; } while (v);
			if (alt && base == 16) { *--p = 'x'; *--p = '0'; }
			if (neg) *--p = '-';
			s = p; len = tmp + sizeof(tmp) - p;
			break;
		}
		case '%': tmp[0] = '%'; len = 1; break;
		default: tmp[0] = '?'; len = 1; break;
		}
		int pad = width > len ? width - len : 0;
		if (!left && zero && (*fmt != 's' && *fmt != 'c')) {
			if (s[0] == '-') { ob_put(&o, '-'); s++; len--; }
			while (pad--) ob_put(&o, '0');
			pad = 0;
		}
		if (!left) while (pad-- > 0) ob_put(&o, ' ');
		for (int i = 0; i < len; i++) ob_put(&o, s[i]);
		if (left) while (pad-- > 0) ob_put(&o, ' ');
	}
	if (o.n) o.buf[o.pos < o.n ? o.pos : o.n - 1] = 0;
	return o.pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return r;
}

int printf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	console_write(buf, strlen(buf));
	return r;
}

void puts_raw(const char *s) { console_write(s, strlen(s)); }

/* ---- heap: first fit with coalescing ---- */
struct blk { size_t size; struct blk *next; int used; u32 magic; };
#define BLK_MAGIC 0x48454150
static struct blk *heap_head;
static uintptr_t heap_lo, heap_hi;

void heap_init(uintptr_t start, uintptr_t end)
{
	start = ALIGN_UP(start, 16);
	heap_lo = start; heap_hi = end;
	heap_head = (struct blk *)start;
	heap_head->size = end - start - sizeof(struct blk);
	heap_head->next = NULL;
	heap_head->used = 0;
	heap_head->magic = BLK_MAGIC;
}

void heap_bounds(uintptr_t *s, uintptr_t *e) { *s = heap_lo; *e = heap_hi; }

void *memalign(size_t align, size_t n)
{
	if (align < 16) align = 16;
	n = ALIGN_UP(n, 16);
	for (struct blk *b = heap_head; b; b = b->next) {
		if (b->used) continue;
		uintptr_t data = (uintptr_t)(b + 1);
		uintptr_t adata = ALIGN_UP(data, align);
		/* need room for a split-off header in front when misaligned */
		if (adata != data) {
			while (adata - data < sizeof(struct blk) + 16) adata += align;
		}
		if (adata + n > data + b->size) continue;
		if (adata != data) {
			struct blk *nb = (struct blk *)(adata - sizeof(struct blk));
			nb->size = data + b->size - adata;
			nb->next = b->next; nb->used = 0; nb->magic = BLK_MAGIC;
			b->size = (uintptr_t)nb - data;
			b->next = nb;
			b = nb;
		}
		if (b->size >= n + sizeof(struct blk) + 32) {
			struct blk *rest = (struct blk *)((uintptr_t)(b + 1) + n);
			rest->size = b->size - n - sizeof(struct blk);
			rest->next = b->next; rest->used = 0; rest->magic = BLK_MAGIC;
			b->size = n; b->next = rest;
		}
		b->used = 1;
		return b + 1;
	}
	return NULL;
}

void *malloc(size_t n) { return memalign(16, n); }
void *zalloc(size_t n) { void *p = malloc(n); if (p) memset(p, 0, n); return p; }

void free(void *p)
{
	if (!p) return;
	struct blk *b = (struct blk *)p - 1;
	if (b->magic != BLK_MAGIC) return;
	b->used = 0;
	for (struct blk *c = heap_head; c; c = c->next) {
		while (!c->used && c->next && !c->next->used &&
		       (uintptr_t)(c + 1) + c->size == (uintptr_t)c->next) {
			c->size += sizeof(struct blk) + c->next->size;
			c->next = c->next->next;
		}
	}
}
