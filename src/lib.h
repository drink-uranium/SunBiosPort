#ifndef LIB_H
#define LIB_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long s64;
typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned long uintptr_t;
typedef int bool;
#define true 1
#define false 0
#define NULL ((void *)0)

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, l) __builtin_va_arg(v, l)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PACKED __attribute__((packed))
#define NORETURN __attribute__((noreturn))

static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port)); }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_delay(void) { outb(0x80, 0); }

static inline u8 read8(uintptr_t a) { return *(volatile u8 *)a; }
static inline u16 read16(uintptr_t a) { return *(volatile u16 *)a; }
static inline u32 read32(uintptr_t a) { return *(volatile u32 *)a; }
static inline void write8(uintptr_t a, u8 v) { *(volatile u8 *)a = v; }
static inline void write16(uintptr_t a, u16 v) { *(volatile u16 *)a = v; }
static inline void write32(uintptr_t a, u32 v) { *(volatile u32 *)a = v; }
static inline void mb(void) { __asm__ volatile("lock; addl $0,0(%%esp)" ::: "memory"); }

static inline void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
	__asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static inline u64 rdtsc(void) { u64 v; __asm__ volatile("rdtsc" : "=A"(v)); return v; }

/* string / memory */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
int tolower(int c);
int toupper(int c);
int isspace(int c);
int isdigit(int c);
int isxdigit(int c);
int isalpha(int c);
int isprint(int c);
unsigned long strtoul(const char *s, char **end, int base);

/* formatted output */
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int printf(const char *fmt, ...);
void puts_raw(const char *s);

/* heap */
void heap_init(uintptr_t start, uintptr_t end);
void *malloc(size_t n);
void *zalloc(size_t n);
void *memalign(size_t align, size_t n);
void free(void *p);
void heap_bounds(uintptr_t *start, uintptr_t *end);

/* setjmp (start.S) */
typedef u32 jmp_buf[6];
int setjmp(jmp_buf b);
NORETURN void longjmp(jmp_buf b, int v);

/* timing (timer.c) */
void timer_init(void);
u64 timer_us(void);
void udelay(u32 us);
void mdelay(u32 ms);
u32 cpu_mhz(void);

#endif
