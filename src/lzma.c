/* LZMA (as used for coreboot CBFS payloads: 13-byte header + raw stream)
 * and LZ4 frame decoders. Written from the format specifications. */
#include "boot.h"

typedef u16 prob_t;
#define PROB_INIT 1024

struct rc { const u8 *in, *end; u32 range, code; int err; };

static u8 rc_byte(struct rc *r)
{
	if (r->in >= r->end) { r->err = 1; return 0; }
	return *r->in++;
}

static void rc_norm(struct rc *r)
{
	if (r->range < (1u << 24)) { r->range <<= 8; r->code = (r->code << 8) | rc_byte(r); }
}

static unsigned rc_bit(struct rc *r, prob_t *p)
{
	u32 v = *p, bound = (r->range >> 11) * v;
	unsigned s;
	if (r->code < bound) { v += (2048 - v) >> 5; r->range = bound; s = 0; }
	else { v -= v >> 5; r->code -= bound; r->range -= bound; s = 1; }
	*p = v;
	rc_norm(r);
	return s;
}

static u32 rc_direct(struct rc *r, int n)
{
	u32 res = 0;
	do {
		r->range >>= 1;
		r->code -= r->range;
		u32 t = 0 - (r->code >> 31);
		r->code += r->range & t;
		rc_norm(r);
		res = (res << 1) + (t + 1);
	} while (--n);
	return res;
}

static u32 tree(struct rc *r, prob_t *p, int n)
{
	u32 m = 1;
	for (int i = 0; i < n; i++) m = (m << 1) + rc_bit(r, p + m);
	return m - (1u << n);
}

static u32 rtree(struct rc *r, prob_t *p, int n)
{
	u32 m = 1, sym = 0;
	for (int i = 0; i < n; i++) {
		unsigned b = rc_bit(r, p + m);
		m = (m << 1) + b;
		sym |= b << i;
	}
	return sym;
}

struct lenc { prob_t choice, choice2, low[16][8], mid[16][8], high[256]; };

static void len_init(struct lenc *l)
{
	prob_t *p = (prob_t *)l;
	for (u32 i = 0; i < sizeof(*l) / 2; i++) p[i] = PROB_INIT;
}

static u32 len_dec(struct rc *r, struct lenc *l, u32 ps)
{
	if (!rc_bit(r, &l->choice)) return tree(r, l->low[ps], 3);
	if (!rc_bit(r, &l->choice2)) return 8 + tree(r, l->mid[ps], 3);
	return 16 + tree(r, l->high, 8);
}

struct lzstate {
	prob_t is_match[192], is_rep[12], is_g0[12], is_g1[12], is_g2[12], is_rep0long[192];
	prob_t pos_slot[4][64], pos_dec[115], align[16];
	struct lenc len, replen;
};

int lzma_decompress(const u8 *src, u32 srclen, u8 *dst, u32 dstlen, u32 *outlen)
{
	if (srclen < 13) return -1;
	u32 props = src[0];
	if (props >= 9 * 5 * 5) return -1;
	u32 lc = props % 9, lp = (props / 9) % 5, pb = props / 45;
	u64 usize = *(const u64 *)(src + 5);
	bool known = usize != ~0ull;
	if (known && usize > dstlen) return -1;
	u32 nlit = 0x300u << (lc + lp);
	prob_t *lit = malloc(nlit * 2);
	struct lzstate *s = malloc(sizeof(*s));
	if (!lit || !s) { free(lit); free(s); return -1; }
	for (u32 i = 0; i < nlit; i++) lit[i] = PROB_INIT;
	prob_t *pp = (prob_t *)s;
	for (u32 i = 0; i < sizeof(*s) / 2; i++) pp[i] = PROB_INIT;
	len_init(&s->len);
	len_init(&s->replen);

	struct rc r = { src + 13, src + srclen, 0xffffffff, 0, 0 };
	rc_byte(&r);
	for (int i = 0; i < 4; i++) r.code = (r.code << 8) | rc_byte(&r);

	u32 out = 0, state = 0, rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
	int ret = 0;
	u32 limit = known ? (u32)usize : dstlen;
	while (out < limit) {
		if (r.err) { ret = -1; break; }
		u32 ps = out & ((1u << pb) - 1);
		if (!rc_bit(&r, &s->is_match[(state << 4) + ps])) {
			u8 prev = out ? dst[out - 1] : 0;
			u32 ls = ((out & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc));
			prob_t *p = lit + 0x300 * ls;
			u32 sym = 1;
			if (state >= 7) {
				u32 mb = dst[out - rep0 - 1];
				do {
					u32 mbit = (mb >> 7) & 1;
					mb <<= 1;
					u32 b = rc_bit(&r, p + ((1 + mbit) << 8) + sym);
					sym = (sym << 1) | b;
					if (mbit != b) break;
				} while (sym < 0x100);
			}
			while (sym < 0x100) sym = (sym << 1) | rc_bit(&r, p + sym);
			dst[out++] = sym - 0x100;
			state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
			continue;
		}
		u32 len;
		if (rc_bit(&r, &s->is_rep[state])) {
			if (!out) { ret = -1; break; }
			if (!rc_bit(&r, &s->is_g0[state])) {
				if (!rc_bit(&r, &s->is_rep0long[(state << 4) + ps])) {
					state = state < 7 ? 9 : 11;
					dst[out] = dst[out - rep0 - 1];
					out++;
					continue;
				}
			} else {
				u32 dist;
				if (!rc_bit(&r, &s->is_g1[state])) dist = rep1;
				else {
					if (!rc_bit(&r, &s->is_g2[state])) dist = rep2;
					else { dist = rep3; rep3 = rep2; }
					rep2 = rep1;
				}
				rep1 = rep0;
				rep0 = dist;
			}
			len = len_dec(&r, &s->replen, ps);
			state = state < 7 ? 8 : 11;
		} else {
			rep3 = rep2; rep2 = rep1; rep1 = rep0;
			len = len_dec(&r, &s->len, ps);
			state = state < 7 ? 7 : 10;
			u32 lstate = len < 3 ? len : 3;
			u32 slot = tree(&r, s->pos_slot[lstate], 6);
			u32 dist;
			if (slot < 4) dist = slot;
			else {
				u32 nd = (slot >> 1) - 1;
				dist = (2 | (slot & 1)) << nd;
				if (slot < 14) dist += rtree(&r, s->pos_dec + dist - slot, nd);
				else {
					dist += rc_direct(&r, nd - 4) << 4;
					dist += rtree(&r, s->align, 4);
				}
			}
			rep0 = dist;
			if (rep0 == 0xffffffff) break;	/* end marker */
			if (rep0 >= out) { ret = -1; break; }
		}
		len += 2;
		if (out + len > dstlen) { ret = -1; break; }
		for (u32 i = 0; i < len && out < limit; i++, out++) dst[out] = dst[out - rep0 - 1];
	}
	free(lit);
	free(s);
	*outlen = out;
	return ret;
}

static int lz4_block(const u8 *src, u32 n, u8 *dst, u32 dstlen, u32 *outlen)
{
	const u8 *ip = src, *iend = src + n;
	u32 op = 0;
	while (ip < iend) {
		u8 tok = *ip++;
		u32 lit = tok >> 4;
		if (lit == 15) { u8 b; do { if (ip >= iend) return -1; b = *ip++; lit += b; } while (b == 255); }
		if (ip + lit > iend || op + lit > dstlen) return -1;
		memcpy(dst + op, ip, lit);
		ip += lit; op += lit;
		if (ip >= iend) break;
		if (ip + 2 > iend) return -1;
		u32 off = ip[0] | (ip[1] << 8);
		ip += 2;
		u32 ml = tok & 15;
		if (ml == 15) { u8 b; do { if (ip >= iend) return -1; b = *ip++; ml += b; } while (b == 255); }
		ml += 4;
		if (!off || off > op || op + ml > dstlen) return -1;
		for (u32 i = 0; i < ml; i++, op++) dst[op] = dst[op - off];
	}
	*outlen = op;
	return 0;
}

int lz4_decompress(const u8 *src, u32 srclen, u8 *dst, u32 dstlen, u32 *outlen)
{
	if (srclen < 7 || *(const u32 *)src != 0x184d2204) return -1;
	u8 flg = src[4];
	const u8 *p = src + 6 + ((flg & 8) ? 8 : 0) + ((flg & 1) ? 4 : 0) + 1;
	const u8 *end = src + srclen;
	u32 out = 0;
	while (p + 4 <= end) {
		u32 bs = *(const u32 *)p;
		p += 4;
		if (!bs) break;
		u32 sz = bs & 0x7fffffff;
		if (p + sz > end) return -1;
		if (bs & 0x80000000) {
			if (out + sz > dstlen) return -1;
			memcpy(dst + out, p, sz);
			out += sz;
		} else {
			u32 got;
			if (lz4_block(p, sz, dst + out, dstlen - out, &got) < 0) return -1;
			out += got;
		}
		p += sz + ((flg & 0x10) ? 4 : 0);
	}
	*outlen = out;
	return 0;
}
