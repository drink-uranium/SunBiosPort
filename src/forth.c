/*
 * The OpenBoot Forth interpreter: an indirect-threaded Forth with 32-bit
 * cells operating directly on physical memory (identity mapped), so that
 * @ ! dump and friends work on real addresses as they do in OBP. The
 * default number base is hex, as in OpenBoot.
 */
#include "forth.h"
#include "console.h"

#define DS_SIZE 512
#define RS_SIZE 512
#define DICT_SIZE (1024 * 1024)

static cell ds[DS_SIZE + 8];
static int sp;
static cell rs[RS_SIZE + 8];
static int rp;
static cell *ip;
static u8 *dict, *here;
static struct word *latest;
static cell state, base = 16;
static jmp_buf abort_jmp;
static bool jmp_valid;

/* input source */
static const char *tib;
static int tin, tlen;

/* compile-time control-flow helpers */
static cell *leave_stack[64];
static int leave_sp;

void fpush(cell v)
{
	if (sp >= DS_SIZE) forth_abort("Stack Overflow");
	ds[sp++] = v;
}

cell fpop(void)
{
	if (sp <= 0) forth_abort("Stack Underflow");
	return ds[--sp];
}

static void rpush(cell v) { if (rp >= RS_SIZE) forth_abort("Return Stack Overflow"); rs[rp++] = v; }
static cell rpop(void) { if (rp <= 0) forth_abort("Return Stack Underflow"); return rs[--rp]; }

static void ctl_reset(void);

NORETURN void forth_abort(const char *msg)
{
	if (msg && *msg) printf("%s\n", msg);
	ctl_reset();
	sp = 0; rp = 0; state = 0;
	if (jmp_valid) longjmp(abort_jmp, 1);
	for (;;) __asm__ volatile("hlt");
}

bool forth_compiling(void) { return state != 0; }

/* ---------------- dictionary ---------------- */
static void comma(cell v)
{
	if (here + sizeof(cell) > dict + DICT_SIZE) forth_abort("Dictionary full");
	*(cell *)here = v;
	here += sizeof(cell);
}

static struct word *new_word(const char *name, size_t len, code_fn fn)
{
	struct word *w = zalloc(sizeof(*w));
	if (!w) forth_abort("Out of memory");
	if (len > 33) len = 33;
	memcpy(w->name, name, len);
	w->name[len] = 0;
	w->len = len;
	w->code = fn;
	w->link = latest;
	latest = w;
	return w;
}

struct word *forth_add_prim(const char *name, code_fn fn, u8 flags, const char *help)
{
	struct word *w = new_word(name, strlen(name), fn);
	w->flags = flags;
	w->help = help;
	return w;
}

static struct word *find(const char *name, size_t len)
{
	for (struct word *w = latest; w; w = w->link) {
		if (w->len != len || (w->flags & F_HIDDEN)) continue;
		if (!strncasecmp(w->name, name, len)) return w;
	}
	return NULL;
}

/* ---------------- inner interpreter ---------------- */
static void run(struct word *w) { w->code(w); }

static void docol(struct word *w)
{
	cell *save = ip;
	ip = (cell *)w->param;
	while (ip) {
		struct word *x = (struct word *)*ip++;
		x->code(x);
	}
	ip = save;
}

static void dovar(struct word *w) { fpush(w->param); }
static void doconst(struct word *w) { fpush(w->param); }
static void dodefer(struct word *w)
{
	if (!w->param) forth_abort("Deferred word not initialized");
	run((struct word *)w->param);
}
static void dodoes(struct word *w)
{
	fpush(w->param);
	cell *save = ip;
	ip = (cell *)w->does;
	while (ip) {
		struct word *x = (struct word *)*ip++;
		x->code(x);
	}
	ip = save;
}

#define PRIM(n) static void n(struct word *w __attribute__((unused)))

PRIM(p_lit) { fpush(*ip++); }
PRIM(p_exit) { ip = NULL; }
PRIM(p_branch) { ip = (cell *)*ip; }
PRIM(p_0branch) { if (!fpop()) ip = (cell *)*ip; else ip++; }
PRIM(p_do) { cell i = fpop(), l = fpop(); rpush(l); rpush(i); }
PRIM(p_qdo)
{
	cell i = fpop(), l = fpop();
	if (i == l) { ip = (cell *)*ip; return; }
	ip++;
	rpush(l); rpush(i);
}
PRIM(p_loop)
{
	cell i = rs[rp - 1] + 1;
	if (i == rs[rp - 2]) { rp -= 2; ip++; }
	else { rs[rp - 1] = i; ip = (cell *)*ip; }
}
PRIM(p_ploop)
{
	cell n = fpop(), old = rs[rp - 1], lim = rs[rp - 2], nw = old + n;
	if (((old - lim) ^ (nw - lim)) < 0) { rp -= 2; ip++; }
	else { rs[rp - 1] = nw; ip = (cell *)*ip; }
}
PRIM(p_leave) { rp -= 2; ip = (cell *)*ip; }
PRIM(p_unloop) { rp -= 2; }
PRIM(p_i) { fpush(rs[rp - 1]); }
PRIM(p_j) { fpush(rs[rp - 3]); }
PRIM(p_dotq)
{
	cell len = *ip++;
	console_write((char *)ip, len);
	ip = (cell *)((u8 *)ip + ALIGN_UP(len, 4));
}
PRIM(p_sq)
{
	cell len = *ip++;
	fpush((cell)ip);
	fpush(len);
	ip = (cell *)((u8 *)ip + ALIGN_UP(len, 4));
}
PRIM(p_abortq)
{
	cell len = *ip++;
	char *s = (char *)ip;
	ip = (cell *)((u8 *)ip + ALIGN_UP(len, 4));
	if (fpop()) {
		char msg[128];
		memcpy(msg, s, MIN(len, 127));
		msg[MIN(len, 127)] = 0;
		forth_abort(msg);
	}
}
PRIM(p_does)
{
	latest->code = dodoes;
	latest->does = (cell)ip;
	ip = NULL;
}
PRIM(p_to) { struct word *x = (struct word *)*ip++; x->param = fpop(); }

static struct word *w_lit, *w_exit, *w_branch, *w_0branch, *w_do, *w_qdo, *w_loop, *w_ploop,
	*w_leave, *w_dotq, *w_sq, *w_abortq, *w_does, *w_to;

static void compile_word(struct word *x) { comma((cell)x); }
static void compile_lit(cell v) { comma((cell)w_lit); comma(v); }

static void compile_string(struct word *op, const char *s, int len)
{
	comma((cell)op);
	comma(len);
	if (here + ALIGN_UP(len, 4) > dict + DICT_SIZE) forth_abort("Dictionary full");
	memcpy(here, s, len);
	here += ALIGN_UP(len, 4);
}

/* ---------------- parsing ---------------- */
static char wordbuf[256];

char *forth_parse_word(void)
{
	while (tin < tlen && isspace((u8)tib[tin])) tin++;
	if (tin >= tlen) return NULL;
	int s = tin;
	while (tin < tlen && !isspace((u8)tib[tin])) tin++;
	int l = MIN(tin - s, 255);
	memcpy(wordbuf, tib + s, l);
	wordbuf[l] = 0;
	if (tin < tlen) tin++;
	return wordbuf;
}

static int parse_delim(char delim, const char **start)
{
	*start = tib + tin;
	int s = tin;
	while (tin < tlen && tib[tin] != delim) tin++;
	int l = tin - s;
	if (tin < tlen) tin++;
	return l;
}

static char restbuf[512];

char *forth_parse_rest(void)
{
	while (tin < tlen && isspace((u8)tib[tin])) tin++;
	int l = MIN(tlen - tin, 511);
	memcpy(restbuf, tib + tin, l);
	restbuf[l] = 0;
	while (l && isspace((u8)restbuf[l - 1])) restbuf[--l] = 0;
	tin = tlen;
	return restbuf;
}

static bool to_number(const char *s, cell *out, int b)
{
	bool neg = false;
	if (*s == '-' && s[1]) { neg = true; s++; }
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && s[2]) { b = 16; s += 2; }
	if (!*s) return false;
	ucell v = 0;
	for (; *s; s++) {
		int d;
		if (*s == '.' || *s == ',') continue;	/* OBP permits punctuation in numbers */
		if (isdigit(*s)) d = *s - '0';
		else if (isalpha(*s)) d = tolower(*s) - 'a' + 10;
		else return false;
		if (d >= b) return false;
		v = v * b + d;
	}
	*out = neg ? -(cell)v : (cell)v;
	return true;
}

/* ---------------- outer interpreter ---------------- */
static void interpret_token(const char *t)
{
	struct word *w = find(t, strlen(t));
	if (w) {
		if (state && !(w->flags & F_IMM)) compile_word(w);
		else run(w);
		return;
	}
	cell v;
	if (to_number(t, &v, base)) {
		if (state) compile_lit(v);
		else fpush(v);
		return;
	}
	char msg[300];
	snprintf(msg, sizeof(msg), "%s ?", t);
	forth_abort(msg);
}

static void interpret_input(void)
{
	char *t;
	while ((t = forth_parse_word())) {
		char tok[256];
		strlcpy(tok, t, sizeof(tok));
		interpret_token(tok);
	}
}

static int with_input(const char *text, int len)
{
	const char *otib = tib;
	int otin = tin, otlen = tlen;
	jmp_buf save;
	bool had = jmp_valid;
	memcpy(save, abort_jmp, sizeof(save));
	tib = text; tin = 0; tlen = len;
	int r = 0;
	jmp_valid = true;
	if (setjmp(abort_jmp) == 0) interpret_input();
	else r = -1;
	memcpy(abort_jmp, save, sizeof(save));
	jmp_valid = had;
	tib = otib; tin = otin; tlen = otlen;
	return r;
}

int forth_interpret(const char *line) { return with_input(line, strlen(line)); }

int forth_evaluate(const char *text)
{
	/* line by line, so that \ comments work */
	const char *p = text;
	while (*p) {
		const char *e = strchr(p, '\n');
		int l = e ? e - p : (int)strlen(p);
		if (with_input(p, l) < 0) return -1;
		if (!e) break;
		p = e + 1;
	}
	return 0;
}

/* ---------------- primitives ---------------- */
#define POP2 cell b_ = fpop(), a_ = fpop()
PRIM(p_add) { POP2; fpush(a_ + b_); }
PRIM(p_sub) { POP2; fpush(a_ - b_); }
PRIM(p_mul) { POP2; fpush(a_ * b_); }
PRIM(p_div) { POP2; if (!b_) forth_abort("Divide by zero"); fpush(a_ / b_); }
PRIM(p_mod) { POP2; if (!b_) forth_abort("Divide by zero"); fpush(a_ % b_); }
PRIM(p_divmod) { POP2; if (!b_) forth_abort("Divide by zero"); fpush(a_ % b_); fpush(a_ / b_); }
PRIM(p_udivmod) { ucell b = fpop(), a = fpop(); if (!b) forth_abort("Divide by zero"); fpush(a % b); fpush(a / b); }
PRIM(p_muldiv) { cell c = fpop(), b = fpop(), a = fpop(); if (!c) forth_abort("Divide by zero"); fpush((cell)(((s64)a * b) / c)); }
PRIM(p_and) { POP2; fpush(a_ & b_); }
PRIM(p_or) { POP2; fpush(a_ | b_); }
PRIM(p_xor) { POP2; fpush(a_ ^ b_); }
PRIM(p_invert) { fpush(~fpop()); }
PRIM(p_negate) { fpush(-fpop()); }
PRIM(p_abs) { cell a = fpop(); fpush(a < 0 ? -a : a); }
PRIM(p_min) { POP2; fpush(a_ < b_ ? a_ : b_); }
PRIM(p_max) { POP2; fpush(a_ > b_ ? a_ : b_); }
PRIM(p_umin) { ucell b = fpop(), a = fpop(); fpush(a < b ? a : b); }
PRIM(p_umax) { ucell b = fpop(), a = fpop(); fpush(a > b ? a : b); }
PRIM(p_lshift) { POP2; fpush((ucell)a_ << b_); }
PRIM(p_rshift) { POP2; fpush((ucell)a_ >> b_); }
PRIM(p_arshift) { POP2; fpush(a_ >> b_); }
PRIM(p_1plus) { fpush(fpop() + 1); }
PRIM(p_1minus) { fpush(fpop() - 1); }
PRIM(p_2plus) { fpush(fpop() + 2); }
PRIM(p_2minus) { fpush(fpop() - 2); }
PRIM(p_2mul) { fpush(fpop() * 2); }
PRIM(p_2div) { fpush(fpop() >> 1); }
PRIM(p_u2div) { fpush((ucell)fpop() >> 1); }
PRIM(p_cells) { fpush(fpop() * 4); }
PRIM(p_cellplus) { fpush(fpop() + 4); }
PRIM(p_eq) { POP2; fpush(a_ == b_ ? -1 : 0); }
PRIM(p_ne) { POP2; fpush(a_ != b_ ? -1 : 0); }
PRIM(p_lt) { POP2; fpush(a_ < b_ ? -1 : 0); }
PRIM(p_gt) { POP2; fpush(a_ > b_ ? -1 : 0); }
PRIM(p_le) { POP2; fpush(a_ <= b_ ? -1 : 0); }
PRIM(p_ge) { POP2; fpush(a_ >= b_ ? -1 : 0); }
PRIM(p_ult) { ucell b = fpop(), a = fpop(); fpush(a < b ? -1 : 0); }
PRIM(p_ugt) { ucell b = fpop(), a = fpop(); fpush(a > b ? -1 : 0); }
PRIM(p_ule) { ucell b = fpop(), a = fpop(); fpush(a <= b ? -1 : 0); }
PRIM(p_uge) { ucell b = fpop(), a = fpop(); fpush(a >= b ? -1 : 0); }
PRIM(p_0eq) { fpush(fpop() == 0 ? -1 : 0); }
PRIM(p_0ne) { fpush(fpop() != 0 ? -1 : 0); }
PRIM(p_0lt) { fpush(fpop() < 0 ? -1 : 0); }
PRIM(p_0gt) { fpush(fpop() > 0 ? -1 : 0); }
PRIM(p_0le) { fpush(fpop() <= 0 ? -1 : 0); }
PRIM(p_0ge) { fpush(fpop() >= 0 ? -1 : 0); }
PRIM(p_within) { ucell h = fpop(), l = fpop(), n = fpop(); fpush(n - l < h - l ? -1 : 0); }
PRIM(p_between) { cell h = fpop(), l = fpop(), n = fpop(); fpush(n >= l && n <= h ? -1 : 0); }

PRIM(p_dup) { cell a = fpop(); fpush(a); fpush(a); }
PRIM(p_qdup) { cell a = fpop(); fpush(a); if (a) fpush(a); }
PRIM(p_drop) { fpop(); }
PRIM(p_swap) { POP2; fpush(b_); fpush(a_); }
PRIM(p_over) { POP2; fpush(a_); fpush(b_); fpush(a_); }
PRIM(p_rot) { cell c = fpop(), b = fpop(), a = fpop(); fpush(b); fpush(c); fpush(a); }
PRIM(p_mrot) { cell c = fpop(), b = fpop(), a = fpop(); fpush(c); fpush(a); fpush(b); }
PRIM(p_nip) { POP2; (void)a_; fpush(b_); }
PRIM(p_tuck) { POP2; fpush(b_); fpush(a_); fpush(b_); }
PRIM(p_pick) { cell n = fpop(); if (n < 0 || n >= sp) forth_abort("Stack Underflow"); fpush(ds[sp - 1 - n]); }
PRIM(p_roll)
{
	cell n = fpop();
	if (n < 0 || n >= sp) forth_abort("Stack Underflow");
	cell v = ds[sp - 1 - n];
	memmove(&ds[sp - 1 - n], &ds[sp - n], n * sizeof(cell));
	ds[sp - 1] = v;
}
PRIM(p_2dup) { POP2; fpush(a_); fpush(b_); fpush(a_); fpush(b_); }
PRIM(p_2drop) { fpop(); fpop(); }
PRIM(p_2swap) { cell d = fpop(), c = fpop(), b = fpop(), a = fpop(); fpush(c); fpush(d); fpush(a); fpush(b); }
PRIM(p_2over) { cell d = fpop(), c = fpop(), b = fpop(), a = fpop(); fpush(a); fpush(b); fpush(c); fpush(d); fpush(a); fpush(b); }
PRIM(p_depth) { fpush(sp); }
PRIM(p_clear) { sp = 0; }
PRIM(p_tor) { rpush(fpop()); }
PRIM(p_rfrom) { fpush(rpop()); }
PRIM(p_rfetch) { if (!rp) forth_abort("Return Stack Underflow"); fpush(rs[rp - 1]); }

PRIM(p_fetch) { fpush(*(cell *)fpop()); }
PRIM(p_store) { cell a = fpop(), v = fpop(); *(cell *)a = v; }
PRIM(p_cfetch) { fpush(*(volatile u8 *)fpop()); }
PRIM(p_cstore) { cell a = fpop(), v = fpop(); *(volatile u8 *)a = v; }
PRIM(p_wfetch) { fpush(*(volatile u16 *)fpop()); }
PRIM(p_wstore) { cell a = fpop(), v = fpop(); *(volatile u16 *)a = v; }
PRIM(p_lfetch) { fpush(*(volatile u32 *)fpop()); }
PRIM(p_lstore) { cell a = fpop(), v = fpop(); *(volatile u32 *)a = v; }
PRIM(p_plstore) { cell a = fpop(), v = fpop(); *(cell *)a += v; }
PRIM(p_2fetch) { cell *a = (cell *)fpop(); fpush(a[1]); fpush(a[0]); }
PRIM(p_2store) { cell *a = (cell *)fpop(); a[0] = fpop(); a[1] = fpop(); }
PRIM(p_move) { cell n = fpop(), d = fpop(), s = fpop(); if (n > 0) memmove((void *)d, (void *)s, n); }
PRIM(p_fill) { cell c = fpop(), n = fpop(), a = fpop(); if (n > 0) memset((void *)a, c, n); }
PRIM(p_erase) { cell n = fpop(), a = fpop(); if (n > 0) memset((void *)a, 0, n); }
PRIM(p_comp)
{
	cell n = fpop(), b = fpop(), a = fpop();
	int r = memcmp((void *)a, (void *)b, n);
	fpush(r < 0 ? -1 : r > 0 ? 1 : 0);
}
PRIM(p_streq)
{
	cell l2 = fpop(), a2 = fpop(), l1 = fpop(), a1 = fpop();
	fpush(l1 == l2 && !memcmp((void *)a1, (void *)a2, l1) ? -1 : 0);
}
PRIM(p_count) { u8 *a = (u8 *)fpop(); fpush((cell)(a + 1)); fpush(*a); }
PRIM(p_bounds) { POP2; fpush(a_ + b_); fpush(a_); }

PRIM(p_here) { fpush((cell)here); }
PRIM(p_allot) { cell n = fpop(); if (here + n > dict + DICT_SIZE) forth_abort("Dictionary full"); here += n; }
PRIM(p_comma) { comma(fpop()); }
PRIM(p_ccomma) { *here++ = fpop(); }
PRIM(p_wcomma) { *(u16 *)here = fpop(); here += 2; }
PRIM(p_align) { here = (u8 *)ALIGN_UP((uintptr_t)here, 4); }
PRIM(p_base) { fpush((cell)&base); }
PRIM(p_state) { fpush((cell)&state); }
PRIM(p_hex) { base = 16; }
PRIM(p_decimal) { base = 10; }
PRIM(p_octal) { base = 8; }
PRIM(p_binary) { base = 2; }

/* number output */
static void print_num(cell v, bool sgn, int b, int width)
{
	char buf[40], *p = buf + sizeof(buf);
	*--p = 0;
	ucell u = (sgn && v < 0) ? -(ucell)v : (ucell)v;
	do { int d = u % b; *--p = d < 10 ? '0' + d : 'a' + d - 10; u /= b; } while (u);
	if (sgn && v < 0) *--p = '-';
	int l = strlen(p);
	for (int i = l; i < width; i++) console_putc(' ');
	console_write(p, l);
}
PRIM(p_dot) { print_num(fpop(), true, base, 0); console_putc(' '); console_flush(); }
PRIM(p_udot) { print_num(fpop(), false, base, 0); console_putc(' '); console_flush(); }
PRIM(p_doth) { print_num(fpop(), false, 16, 0); console_putc(' '); console_flush(); }
PRIM(p_dotd) { print_num(fpop(), true, 10, 0); console_putc(' '); console_flush(); }
PRIM(p_dotr) { cell w_ = fpop(); print_num(fpop(), true, base, w_); console_flush(); }
PRIM(p_udotr) { cell w_ = fpop(); print_num(fpop(), false, base, w_); console_flush(); }
PRIM(p_dots)
{
	if (!sp) { printf("Empty\n"); return; }
	for (int i = 0; i < sp; i++) { print_num(ds[i], false, base, 0); console_putc(' '); }
	console_putc('\n');
	console_flush();
}
PRIM(p_emit) { console_putc(fpop()); console_flush(); }
PRIM(p_type) { cell n = fpop(), a = fpop(); if (n > 0) console_write((char *)a, n); }
PRIM(p_cr) { console_putc('\n'); console_flush(); }
PRIM(p_space) { console_putc(' '); console_flush(); }
PRIM(p_spaces) { cell n = fpop(); while (n-- > 0) console_putc(' '); console_flush(); }
PRIM(p_key);
static int pending_key = -1;
PRIM(p_keyq) { if (pending_key < 0) pending_key = console_getc_nb(); fpush(pending_key >= 0 ? -1 : 0); }
PRIM(p_accept)
{
	cell n = fpop(), a = fpop();
	char buf[256];
	int l = readline(NULL, buf, MIN((int)sizeof(buf), (int)n + 1));
	memcpy((void *)a, buf, l);
	fpush(l);
}
PRIM(p_key)
{
	if (pending_key >= 0) { fpush(pending_key); pending_key = -1; return; }
	fpush(console_getc());
}
PRIM(p_bl) { fpush(' '); }
PRIM(p_true) { fpush(-1); }
PRIM(p_false) { fpush(0); }

PRIM(p_execute) { run((struct word *)fpop()); }
PRIM(p_tick)
{
	char *t = forth_parse_word();
	if (!t) forth_abort("Missing name");
	struct word *x = find(t, strlen(t));
	if (!x) { char m[300]; snprintf(m, sizeof(m), "%s ?", t); forth_abort(m); }
	fpush((cell)x);
}
PRIM(p_btick) { p_tick(w); compile_lit(fpop()); }
PRIM(p_compilecomma) { comma(fpop()); }
PRIM(p_literal) { compile_lit(fpop()); }
PRIM(p_lbracket) { state = 0; }
PRIM(p_rbracket) { state = 1; }
PRIM(p_immediate) { latest->flags |= F_IMM; }

PRIM(p_colon)
{
	char *t = forth_parse_word();
	if (!t) forth_abort("Missing name");
	here = (u8 *)ALIGN_UP((uintptr_t)here, 4);
	struct word *x = new_word(t, strlen(t), docol);
	x->param = (cell)here;
	x->flags |= F_HIDDEN;
	state = 1;
	leave_sp = 0;
	fpush(0x434f4c4e);	/* colon-sys marker */
}
PRIM(p_semicolon)
{
	if (fpop() != 0x434f4c4e) forth_abort("Control structure mismatch");
	compile_word(w_exit);
	latest->flags &= ~F_HIDDEN;
	latest->does = (cell)here;	/* end of body, for "see" */
	state = 0;
}
PRIM(p_recurse) { compile_word(latest); }
PRIM(p_create)
{
	char *t = forth_parse_word();
	if (!t) forth_abort("Missing name");
	here = (u8 *)ALIGN_UP((uintptr_t)here, 4);
	struct word *x = new_word(t, strlen(t), dovar);
	x->param = (cell)here;
}
PRIM(p_doescomp) { compile_word(w_does); }
PRIM(p_variable) { p_create(w); comma(0); }
PRIM(p_constant)
{
	cell v = fpop();
	char *t = forth_parse_word();
	if (!t) forth_abort("Missing name");
	struct word *x = new_word(t, strlen(t), doconst);
	x->param = v;
}
PRIM(p_value) { p_constant(w); }
PRIM(p_defer)
{
	char *t = forth_parse_word();
	if (!t) forth_abort("Missing name");
	new_word(t, strlen(t), dodefer);
}
static struct word *parse_find(void)
{
	char *t = forth_parse_word();
	if (!t) forth_abort("Missing name");
	struct word *x = find(t, strlen(t));
	if (!x) { char m[300]; snprintf(m, sizeof(m), "%s ?", t); forth_abort(m); }
	return x;
}
PRIM(p_toval)
{
	struct word *x = parse_find();
	if (state) { compile_word(w_to); comma((cell)x); }
	else x->param = fpop();
}
PRIM(p_is) { p_toval(w); }
PRIM(p_postpone)
{
	struct word *x = parse_find();
	if (x->flags & F_IMM) compile_word(x);
	else { compile_lit((cell)x); compile_word(find("compile,", 8)); }
}

/*
 * Control structures typed at the ok prompt (e.g. "10 0 do i . loop") are
 * compiled into a scratch buffer and run as soon as they are closed, as
 * OpenBoot does.
 */
static u8 tmpbuf[8192];
static u8 *tmp_saved_here;
static int tmp_depth;
static bool tmp_active;

static void ctl_open(void)
{
	if (!state && !tmp_active) {
		tmp_active = true;
		tmp_depth = 0;
		tmp_saved_here = here;
		here = tmpbuf;
		leave_sp = 0;
		state = 1;
	}
	if (tmp_active) tmp_depth++;
}

static void ctl_close(void)
{
	if (!tmp_active || --tmp_depth > 0) return;
	compile_word(w_exit);
	tmp_active = false;
	state = 0;
	here = tmp_saved_here;
	struct word tmp = { .code = docol, .param = (cell)tmpbuf };
	docol(&tmp);
}

static void ctl_reset(void)
{
	if (tmp_active) { here = tmp_saved_here; tmp_active = false; }
}

/* control flow (compile-time, immediate) */
static void compile_if(void) { compile_word(w_0branch); fpush((cell)here); comma(0); }
PRIM(p_if) { ctl_open(); compile_if(); }
PRIM(p_else)
{
	cell *orig = (cell *)fpop();
	compile_word(w_branch);
	fpush((cell)here);
	comma(0);
	*orig = (cell)here;
}
PRIM(p_then) { cell *orig = (cell *)fpop(); *orig = (cell)here; ctl_close(); }
PRIM(p_begin) { ctl_open(); fpush((cell)here); }
PRIM(p_until) { compile_word(w_0branch); comma(fpop()); ctl_close(); }
PRIM(p_again) { compile_word(w_branch); comma(fpop()); ctl_close(); }
PRIM(p_while) { cell dest = fpop(); compile_if(); fpush(dest); }
PRIM(p_repeat)
{
	cell dest = fpop();
	cell *orig = (cell *)fpop();
	compile_word(w_branch);
	comma(dest);
	*orig = (cell)here;
	ctl_close();
}
PRIM(p_docomp)
{
	ctl_open();
	compile_word(w_do);
	leave_stack[leave_sp++] = NULL;
	fpush((cell)here);
}
PRIM(p_qdocomp)
{
	ctl_open();
	compile_word(w_qdo);
	leave_stack[leave_sp++] = NULL;
	leave_stack[leave_sp++] = (cell *)here;	/* exit target of ?do */
	comma(0);
	fpush((cell)here);
}
static void resolve_leaves(void)
{
	while (leave_sp > 0) {
		cell *a = leave_stack[--leave_sp];
		if (!a) break;
		*a = (cell)here;
	}
}
PRIM(p_loopcomp) { compile_word(w_loop); comma(fpop()); resolve_leaves(); ctl_close(); }
PRIM(p_ploopcomp) { compile_word(w_ploop); comma(fpop()); resolve_leaves(); ctl_close(); }
PRIM(p_leavecomp)
{
	compile_word(w_leave);
	if (leave_sp >= 63) forth_abort("Too many LEAVEs");
	leave_stack[leave_sp++] = (cell *)here;
	comma(0);
}
PRIM(p_exitcomp) { compile_word(w_exit); }
PRIM(p_qleavecomp)
{
	compile_word(w_0branch);
	cell *skip = (cell *)here;
	comma(0);
	p_leavecomp(w);
	*skip = (cell)here;
}
PRIM(p_tobody) { fpush(((struct word *)fpop())->param); }
PRIM(p_allocmem) { void *p = malloc(fpop()); if (!p) forth_abort("Out of memory"); fpush((cell)p); }
PRIM(p_freemem) { fpop(); free((void *)fpop()); }

/* strings and comments */
PRIM(p_paren) { const char *s; parse_delim(')', &s); }
PRIM(p_backslash) { tin = tlen; }
PRIM(p_dotparen) { const char *s; int l = parse_delim(')', &s); console_write(s, l); }
static char strbufs[4][256];
static int strbuf_i;
static int parse_obp_string(char *out, int max)
{
	/* OBP " string: "( hex )" escapes and "" for a quote */
	int n = 0;
	while (tin < tlen) {
		char c = tib[tin++];
		if (c != '"') { if (n < max) out[n++] = c; continue; }
		if (tin < tlen && tib[tin] == '(') {
			tin++;
			while (tin < tlen && tib[tin] != ')') {
				while (tin < tlen && isspace((u8)tib[tin])) tin++;
				if (tin < tlen && isxdigit((u8)tib[tin]) && tin + 1 < tlen && isxdigit((u8)tib[tin + 1])) {
					char h[3] = { tib[tin], tib[tin + 1], 0 };
					if (n < max) out[n++] = strtoul(h, NULL, 16);
					tin += 2;
				} else if (tin < tlen && tib[tin] != ')') tin++;
			}
			if (tin < tlen) tin++;
			continue;
		}
		if (tin < tlen && tib[tin] == '"') { if (n < max) out[n++] = '"'; tin++; continue; }
		break;
	}
	return n;
}
static void string_literal(bool obp)
{
	char tmp[256];
	int l;
	if (obp) l = parse_obp_string(tmp, sizeof(tmp));
	else { const char *s; l = parse_delim('"', &s); l = MIN(l, 255); memcpy(tmp, s, l); }
	if (state) { compile_string(w_sq, tmp, l); return; }
	char *b = strbufs[strbuf_i++ & 3];
	memcpy(b, tmp, l);
	fpush((cell)b);
	fpush(l);
}
PRIM(p_quote) { string_literal(true); }
PRIM(p_squote) { string_literal(false); }
PRIM(p_dotquote)
{
	const char *s;
	int l = parse_delim('"', &s);
	if (state) compile_string(w_dotq, s, l);
	else console_write(s, l);
}
PRIM(p_abortquote)
{
	const char *s;
	int l = parse_delim('"', &s);
	compile_string(w_abortq, s, l);
}
PRIM(p_abort) { forth_abort(NULL); }
PRIM(p_char) { char *t = forth_parse_word(); fpush(t ? (u8)t[0] : 0); }
PRIM(p_bchar) { char *t = forth_parse_word(); compile_lit(t ? (u8)t[0] : 0); }
static void prefixed_number(int b)
{
	char *t = forth_parse_word();
	cell v;
	if (!t || !to_number(t, &v, b)) forth_abort("Bad number");
	if (state) compile_lit(v);
	else fpush(v);
}
PRIM(p_dnum) { prefixed_number(10); }
PRIM(p_hnum) { prefixed_number(16); }
PRIM(p_onum) { prefixed_number(8); }
PRIM(p_bnum) { prefixed_number(2); }
PRIM(p_evaluate)
{
	cell n = fpop(), a = fpop();
	char *buf = malloc(n + 1);
	memcpy(buf, (void *)a, n);
	buf[n] = 0;
	int r = forth_evaluate(buf);
	free(buf);
	if (r < 0) forth_abort(NULL);
}

PRIM(p_words)
{
	int col = 0;
	for (struct word *x = latest; x; x = x->link) {
		if (x->flags & F_HIDDEN || x->name[0] == '(' ) continue;
		int l = x->len;
		if (col + l + 1 > 78) { console_putc('\n'); col = 0; }
		console_write(x->name, l);
		console_putc(' ');
		col += l + 1;
	}
	console_putc('\n');
	console_flush();
}

static const char *name_of(struct word *x) { return x ? x->name : "?"; }

PRIM(p_see)
{
	struct word *x = parse_find();
	if (x->code != docol) {
		if (x->code == dovar) printf("variable %s\n", x->name);
		else if (x->code == doconst) printf("%x constant %s\n", x->param, x->name);
		else if (x->code == dodefer) printf("defer %s is %s\n", x->name, name_of((struct word *)x->param));
		else printf("code %s%s%s\n", x->name, x->help ? "  \\ " : "", x->help ? x->help : "");
		return;
	}
	printf(": %s\n   ", x->name);
	int col = 3;
	cell *end = (cell *)x->does;
	for (cell *p = (cell *)x->param; p < end; p++) {
		struct word *y = (struct word *)*p;
		char buf[64];
		if (y == w_exit && p + 1 >= end) break;
		if (y == w_lit) snprintf(buf, sizeof(buf), "%x", *++p);
		else if (y == w_branch || y == w_0branch || y == w_loop || y == w_ploop || y == w_qdo || y == w_leave) {
			snprintf(buf, sizeof(buf), "%s", y->name);
			p++;
		} else if (y == w_dotq || y == w_sq || y == w_abortq) {
			cell l = *++p;
			snprintf(buf, sizeof(buf), "%s %.*s\"", y == w_dotq ? ".\"" : y == w_sq ? "\"" : "abort\"", (int)MIN(l, 40), (char *)(p + 1));
			p = (cell *)((u8 *)(p + 1) + ALIGN_UP(l, 4)) - 1;
		} else if (y == w_exit) {
			snprintf(buf, sizeof(buf), "exit");
		} else if (y == w_to) {
			snprintf(buf, sizeof(buf), "to %s", name_of((struct word *)*++p));
		} else {
			snprintf(buf, sizeof(buf), "%s", y->name);
		}
		int l = strlen(buf);
		if (col + l > 76) { printf("\n   "); col = 3; }
		printf("%s ", buf);
		col += l + 1;
	}
	printf("\n;\n");
}

PRIM(p_dump)
{
	cell len = fpop();
	u8 *a = (u8 *)fpop();
	u8 *start = (u8 *)((uintptr_t)a & ~15u);
	printf("          0  1  2  3  4  5  6  7   8  9  a  b  c  d  e  f  0123456789abcdef\n");
	for (u8 *row = start; row < a + len; row += 16) {
		char line[100];
		int n = snprintf(line, sizeof(line), "%8x ", (u32)(uintptr_t)row);
		for (int i = 0; i < 16; i++) {
			u8 *b = row + i;
			if (b < a || b >= a + len) n += snprintf(line + n, sizeof(line) - n, "   ");
			else n += snprintf(line + n, sizeof(line) - n, "%02x ", *b);
			if (i == 7) line[n++] = ' ';
		}
		line[n++] = ' ';
		for (int i = 0; i < 16; i++) {
			u8 *b = row + i;
			line[n++] = (b < a || b >= a + len) ? ' ' : (isprint(*b) ? *b : '.');
		}
		line[n] = 0;
		printf("%s\n", line);
		int c = console_getc_nb();
		if (c == 3 || c == 'q') break;
	}
}

PRIM(p_ms) { mdelay(fpop()); }
PRIM(p_us) { udelay(fpop()); }
PRIM(p_getmsecs) { fpush((cell)(timer_us() / 1000)); }
PRIM(p_inb) { fpush(inb(fpop())); }
PRIM(p_inw) { fpush(inw(fpop())); }
PRIM(p_inl) { fpush(inl(fpop())); }
PRIM(p_outb) { cell p = fpop(); outb(p, fpop()); }
PRIM(p_outw) { cell p = fpop(); outw(p, fpop()); }
PRIM(p_outl) { cell p = fpop(); outl(p, fpop()); }

struct pinit { const char *name; code_fn fn; u8 flags; };

static const struct pinit prims[] = {
	{ "+", p_add }, { "-", p_sub }, { "*", p_mul }, { "/", p_div }, { "mod", p_mod },
	{ "/mod", p_divmod }, { "u/mod", p_udivmod }, { "*/", p_muldiv },
	{ "and", p_and }, { "or", p_or }, { "xor", p_xor }, { "invert", p_invert }, { "not", p_invert },
	{ "negate", p_negate }, { "abs", p_abs }, { "min", p_min }, { "max", p_max },
	{ "umin", p_umin }, { "umax", p_umax }, { "lshift", p_lshift }, { "rshift", p_rshift },
	{ "<<", p_lshift }, { ">>", p_rshift }, { ">>a", p_arshift },
	{ "1+", p_1plus }, { "1-", p_1minus }, { "2+", p_2plus }, { "2-", p_2minus },
	{ "2*", p_2mul }, { "2/", p_2div }, { "u2/", p_u2div }, { "cells", p_cells }, { "cell+", p_cellplus },
	{ "/n*", p_cells }, { "na+", p_add },
	{ "=", p_eq }, { "<>", p_ne }, { "<", p_lt }, { ">", p_gt }, { "<=", p_le }, { ">=", p_ge },
	{ "u<", p_ult }, { "u>", p_ugt }, { "u<=", p_ule }, { "u>=", p_uge },
	{ "0=", p_0eq }, { "0<>", p_0ne }, { "0<", p_0lt }, { "0>", p_0gt }, { "0<=", p_0le }, { "0>=", p_0ge },
	{ "within", p_within }, { "between", p_between },
	{ "dup", p_dup }, { "?dup", p_qdup }, { "drop", p_drop }, { "swap", p_swap }, { "over", p_over },
	{ "rot", p_rot }, { "-rot", p_mrot }, { "nip", p_nip }, { "tuck", p_tuck }, { "pick", p_pick },
	{ "roll", p_roll }, { "2dup", p_2dup }, { "2drop", p_2drop }, { "2swap", p_2swap }, { "2over", p_2over },
	{ "depth", p_depth }, { "clear", p_clear }, { ">r", p_tor }, { "r>", p_rfrom }, { "r@", p_rfetch },
	{ "@", p_fetch }, { "!", p_store }, { "c@", p_cfetch }, { "c!", p_cstore }, { "w@", p_wfetch },
	{ "w!", p_wstore }, { "l@", p_lfetch }, { "l!", p_lstore }, { "+!", p_plstore },
	{ "2@", p_2fetch }, { "2!", p_2store }, { "move", p_move }, { "cmove", p_move }, { "fill", p_fill },
	{ "erase", p_erase }, { "comp", p_comp }, { "$=", p_streq }, { "count", p_count }, { "bounds", p_bounds },
	{ "here", p_here }, { "allot", p_allot }, { ",", p_comma }, { "c,", p_ccomma }, { "w,", p_wcomma },
	{ "l,", p_comma }, { "align", p_align }, { "base", p_base }, { "state", p_state },
	{ "hex", p_hex }, { "decimal", p_decimal }, { "octal", p_octal }, { "binary", p_binary },
	{ ".", p_dot }, { "u.", p_udot }, { ".h", p_doth }, { ".d", p_dotd }, { ".r", p_dotr },
	{ "u.r", p_udotr }, { ".s", p_dots }, { "emit", p_emit }, { "type", p_type }, { "cr", p_cr },
	{ "space", p_space }, { "spaces", p_spaces }, { "key", p_key }, { "key?", p_keyq },
	{ "accept", p_accept }, { "expect", p_accept }, { "bl", p_bl }, { "true", p_true }, { "false", p_false },
	{ "execute", p_execute }, { "'", p_tick }, { "[']", p_btick, F_IMM }, { "compile,", p_compilecomma },
	{ "literal", p_literal, F_IMM }, { "[", p_lbracket, F_IMM }, { "]", p_rbracket },
	{ "immediate", p_immediate }, { ":", p_colon }, { ";", p_semicolon, F_IMM },
	{ "recurse", p_recurse, F_IMM }, { "create", p_create }, { "does>", p_doescomp, F_IMM },
	{ "variable", p_variable }, { "constant", p_constant }, { "value", p_value }, { "defer", p_defer },
	{ "to", p_toval, F_IMM }, { "is", p_is, F_IMM }, { "postpone", p_postpone, F_IMM },
	{ "[compile]", p_postpone, F_IMM },
	{ "if", p_if, F_IMM }, { "else", p_else, F_IMM }, { "then", p_then, F_IMM }, { "endif", p_then, F_IMM },
	{ "begin", p_begin, F_IMM }, { "until", p_until, F_IMM }, { "again", p_again, F_IMM },
	{ "while", p_while, F_IMM }, { "repeat", p_repeat, F_IMM }, { "do", p_docomp, F_IMM },
	{ "?do", p_qdocomp, F_IMM }, { "loop", p_loopcomp, F_IMM }, { "+loop", p_ploopcomp, F_IMM },
	{ "leave", p_leavecomp, F_IMM }, { "?leave", p_qleavecomp, F_IMM }, { ">body", p_tobody },
	{ "alloc-mem", p_allocmem }, { "free-mem", p_freemem }, { "i", p_i }, { "j", p_j },
	{ "unloop", p_unloop }, { "exit", p_exitcomp, F_IMM },
	{ "(", p_paren, F_IMM }, { "\\", p_backslash, F_IMM }, { ".(", p_dotparen, F_IMM },
	{ "\"", p_quote, F_IMM }, { "s\"", p_squote, F_IMM }, { ".\"", p_dotquote, F_IMM },
	{ "abort\"", p_abortquote, F_IMM }, { "abort", p_abort }, { "char", p_char },
	{ "[char]", p_bchar, F_IMM }, { "ascii", p_char },
	{ "d#", p_dnum, F_IMM }, { "h#", p_hnum, F_IMM }, { "o#", p_onum, F_IMM }, { "b#", p_bnum, F_IMM },
	{ "evaluate", p_evaluate }, { "words", p_words }, { "see", p_see }, { "dump", p_dump },
	{ "ms", p_ms }, { "us", p_us }, { "get-msecs", p_getmsecs },
	{ "inb", p_inb }, { "inw", p_inw }, { "inl", p_inl }, { "outb", p_outb }, { "outw", p_outw },
	{ "outl", p_outl }, { "pc@", p_inb }, { "pw@", p_inw }, { "pl@", p_inl }, { "pc!", p_outb },
	{ "pw!", p_outw }, { "pl!", p_outl },
};

/* High-level definitions, written in Forth as OpenBoot does. */
static const char prelude[] =
	": 2>r  swap >r >r ;  : 2r>  r> r> swap ;\n"
	": /n  4 ;  : /l  4 ;  : /w  2 ;  : /c  1 ;  : cell  4 ;\n"
	": ca+  + ;  : wa+  2* + ;  : la+  4 * + ;  : char+  1+ ;  : chars  ;\n"
	": blank  bl fill ;  : off  0 swap ! ;  : on  -1 swap ! ;\n"
	": +1  1+ ;  : u*  * ;  : 3dup  2 pick 2 pick 2 pick ;  : 3drop  2drop drop ;\n"
	": lbsplit ( l -- b0 b1 b2 b3 ) dup ff and swap 8 rshift dup ff and swap 8 rshift dup ff and swap 8 rshift ;\n"
	": wbsplit ( w -- b0 b1 ) dup ff and swap 8 rshift ff and ;\n"
	": bwjoin ( b0 b1 -- w ) 8 lshift or ;  : wljoin ( w0 w1 -- l ) 10 lshift or ;\n"
	": lwsplit ( l -- w0 w1 ) dup ffff and swap 10 rshift ;\n"
	": noop  ;\n"
	": sign? 0< ;  : ?negate  0< if negate then ;\n"
	": /string ( a l n -- a+n l-n ) tuck - >r + r> ;\n";

void forth_init(void)
{
	dict = malloc(DICT_SIZE);
	if (!dict) return;
	here = dict;
	w_lit = forth_add_prim("(lit)", p_lit, F_HIDDEN, NULL);
	w_exit = forth_add_prim("(exit)", p_exit, F_HIDDEN, NULL);
	w_branch = forth_add_prim("branch", p_branch, F_HIDDEN, NULL);
	w_0branch = forth_add_prim("?branch", p_0branch, F_HIDDEN, NULL);
	w_do = forth_add_prim("(do)", p_do, F_HIDDEN, NULL);
	w_qdo = forth_add_prim("(?do)", p_qdo, F_HIDDEN, NULL);
	w_loop = forth_add_prim("(loop)", p_loop, F_HIDDEN, NULL);
	w_ploop = forth_add_prim("(+loop)", p_ploop, F_HIDDEN, NULL);
	w_leave = forth_add_prim("(leave)", p_leave, F_HIDDEN, NULL);
	w_dotq = forth_add_prim("(.\")", p_dotq, F_HIDDEN, NULL);
	w_sq = forth_add_prim("(\")", p_sq, F_HIDDEN, NULL);
	w_abortq = forth_add_prim("(abort\")", p_abortq, F_HIDDEN, NULL);
	w_does = forth_add_prim("(does>)", p_does, F_HIDDEN, NULL);
	w_to = forth_add_prim("(to)", p_to, F_HIDDEN, NULL);
	for (u32 i = 0; i < ARRAY_SIZE(prims); i++)
		if (prims[i].fn) forth_add_prim(prims[i].name, prims[i].fn, prims[i].flags, NULL);
	obp_words_init();
	forth_evaluate(prelude);
}

void forth_quit(void)
{
	char line[512];
	for (;;) {
		readline(state ? "] " : "ok ", line, sizeof(line));
		forth_interpret(line);
	}
}
