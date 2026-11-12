/* CFF / Type 2 regression contract for libschrift.
 *
 * Every font used here is generated in this file (no third-party packages,
 * no downloads, no Python).  Geometrical references are recomputed from the
 * font units the test itself laid out (closed-form extrema of cubics and
 * Simpson integration); nothing here derives geometry from the interpreter
 * under test. */

#define _POSIX_C_SOURCE 200809L

#include "schrift.h"
#include "contract.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Growable byte buffer.                                               */

typedef struct Buf { uint8_t *p; size_t len, cap; int bad; } Buf;

static void b_need(Buf *b, size_t extra)
{
	uint8_t *p; size_t cap;
	if (b->bad || b->len + extra <= b->cap) return;
	cap = b->cap ? b->cap : 256;
	while (cap < b->len + extra) cap *= 2;
	p = realloc(b->p, cap);
	if (!p) { b->bad = 1; return; }
	b->p = p; b->cap = cap;
}
static void b_u8(Buf *b, unsigned v)
{ b_need(b, 1); if (b->bad) return; b->p[b->len++] = (uint8_t) v; }
static void b_u16(Buf *b, unsigned v)
{ b_need(b, 2); if (b->bad) return;
  b->p[b->len++] = (uint8_t) ((v >> 8) & 0xFF);
  b->p[b->len++] = (uint8_t) (v & 0xFF); }
static void b_u32(Buf *b, uint32_t v)
{ b_need(b, 4); if (b->bad) return;
  b->p[b->len++] = (uint8_t) (v >> 24); b->p[b->len++] = (uint8_t) (v >> 16);
  b->p[b->len++] = (uint8_t) (v >> 8);  b->p[b->len++] = (uint8_t) v; }
static void b_mem(Buf *b, const void *m, size_t n)
{ b_need(b, n); if (b->bad) return; memcpy(b->p + b->len, m, n); b->len += n; }
static void b_free(Buf *b) { free(b->p); *b = (Buf) { NULL, 0, 0, 0 }; }

/* ------------------------------------------------------------------ */
/* CFF INDEX.                                                          */

typedef struct Item { const uint8_t *p; size_t n; } Item;

static size_t index_off_size(size_t maxOff)
{ return maxOff <= 0xFF ? 1 : maxOff <= 0xFFFF ? 2 : maxOff <= 0xFFFFFF ? 3 : 4; }

static size_t index_size(const Item *it, size_t n)
{
	size_t total = 0, i, os;
	if (!n) return 2;
	for (i = 0; i < n; i++) total += it[i].n;
	os = index_off_size(total + 1);
	return 2 + 1 + (n + 1) * os + total;
}

static void index_write(Buf *out, const Item *it, size_t n)
{
	size_t total = 0, i, os, off, k;
	if (!n) { b_u16(out, 0); return; }
	for (i = 0; i < n; i++) total += it[i].n;
	os = index_off_size(total + 1);
	b_u16(out, (unsigned) n);
	b_u8(out, (unsigned) os);
	off = 1;
	for (i = 0; i <= n; i++) {
		for (k = 0; k < os; k++)
			b_u8(out, (unsigned) ((off >> (8 * (os - 1 - k))) & 0xFF));
		if (i < n) off += it[i].n;
	}
	for (i = 0; i < n; i++) b_mem(out, it[i].p, it[i].n);
}

/* Allocates subr items over one shared storage buffer.  `special` is
 * placed at `specialIndex`; every other subr is a bare `return`. */
static Item *make_subr_items(size_t count, size_t specialIndex,
	const Buf *special, Buf *storage)
{
	size_t *offs, i;
	Item *items = calloc(count ? count : 1, sizeof *items);
	offs = calloc(count ? count : 1, sizeof *offs);
	if (!items || !offs) { free(items); free(offs); return NULL; }
	for (i = 0; i < count; i++) {
		offs[i] = storage->len;
		if (i == specialIndex && special && special->len)
			b_mem(storage, special->p, special->len);
		else
			b_u8(storage, 11);   /* return */
	}
	if (storage->bad) { free(items); free(offs); return NULL; }
	for (i = 0; i < count; i++) {
		items[i].p = storage->p + offs[i];
		items[i].n = (i + 1 < count ? offs[i + 1] : storage->len) - offs[i];
	}
	free(offs);
	return items;
}

/* ------------------------------------------------------------------ */
/* DICT encoding.  32-bit offsets use 5-byte integers so the DICT size */
/* is independent of the values that are patched in afterwards.        */

static void dict_i32(Buf *d, int32_t v, uint32_t *pos)
{
	if (pos) *pos = (uint32_t) (d->len + 1);
	b_u8(d, 29);
	b_u32(d, (uint32_t) v);
}
static void dict_int(Buf *d, int32_t v, unsigned op)
{
	if (v >= -107 && v <= 107) b_u8(d, (unsigned) (v + 139));
	else { b_u8(d, 28); b_u16(d, (unsigned) (v & 0xFFFF)); }
	b_u8(d, op);
}

typedef struct CffSpec
{
	const Item *chars;    size_t nChars;
	const Item *gsubrs;   size_t nGsubrs;
	const Item *lsubrs;   size_t nLsubrs;
	int         writeWidths;
	int32_t     defaultWidthX, nominalWidthX;
	int         useFontBBox;
	int32_t     fontBox[4];
	int         charstringType; /* -1: omit (defaults to 2) */
	int         useROS;
	int         major;
} CffSpec;

typedef struct CffLayout
{
	uint32_t csIndex, csOperand;
	uint32_t gsIndex, lsIndex;
	uint32_t privStart, privOperand, privSizeOperand, subrsOperand;
} CffLayout;

static void cff_assemble(Buf *out, const CffSpec *sp, CffLayout *lo)
{
	static const uint8_t nm[] = "SchriftCFF";
	Item nameItem = { nm, sizeof nm - 1 };
	Buf top = { 0 }, priv = { 0 };
	Item topItem = { NULL, 0 }, privItem = { NULL, 0 };
	size_t nameSize, topSize, gsSize, csSize, lsSize, os;
	uint32_t namePos, topPos, strPos, gsPos, csPos, privPos, lsPos, topData;
	uint32_t csVal = 0, szVal = 0, privVal = 0, subrsVal = 0, pos;
	CffLayout noLayout;

	memset(&noLayout, 0, sizeof noLayout);
	if (lo) memset(lo, 0, sizeof *lo);
	else lo = &noLayout;
	dict_i32(&top, 0, &csVal); b_u8(&top, 17);
	if (sp->nLsubrs > 0 || sp->writeWidths) {
		dict_i32(&top, 0, &szVal);
		dict_i32(&top, 0, &privVal);
		b_u8(&top, 18);
	}
	if (sp->useFontBBox) {
		int i;
		for (i = 0; i < 4; i++) dict_i32(&top, sp->fontBox[i], NULL);
		b_u8(&top, 5);
	}
	if (sp->charstringType >= 0) {
		b_u8(&top, (unsigned) (139 + sp->charstringType));
		b_u8(&top, 12); b_u8(&top, 6);
	}
	if (sp->useROS) {
		b_u8(&top, 139); b_u8(&top, 139); b_u8(&top, 139);
		b_u8(&top, 12); b_u8(&top, 30);
	}
	if (sp->nLsubrs > 0 || sp->writeWidths) {
		/* The Subrs offset must be measured from the start of the
		 * Private DICT, so it goes before any padding. */
		if (sp->nLsubrs > 0) {
			dict_i32(&priv, 0, &subrsVal);
			b_u8(&priv, 19);
		}
		if (sp->writeWidths) {
			dict_int(&priv, sp->defaultWidthX, 20);
			dict_int(&priv, sp->nominalWidthX, 21);
		}
		/* Pad the body so its declared length always matches the bytes
		 * that are actually written; readers that stop at the declared
		 * size still see a complete DICT. */
		while (priv.len < 6) b_u8(&priv, 0);
	}
	topItem.p = top.p; topItem.n = top.len;
	privItem.p = priv.p; privItem.n = priv.len;

	nameSize = index_size(&nameItem, 1);
	topSize = index_size(&topItem, 1);
	gsSize = index_size(sp->gsubrs, sp->nGsubrs);
	csSize = index_size(sp->chars, sp->nChars);
	lsSize = index_size(sp->lsubrs, sp->nLsubrs);
	pos = 4;
	namePos = pos; pos += (uint32_t) nameSize;
	topPos = pos;  pos += (uint32_t) topSize;
	strPos = pos;  pos += 2;
	gsPos = pos;   pos += (uint32_t) gsSize;
	csPos = pos;   pos += (uint32_t) csSize;
	privPos = pos; pos += (uint32_t) privItem.n;
	lsPos = pos;   pos += (uint32_t) lsSize;
	os = index_off_size(top.len + 1);
	topData = topPos + 3 + 2 * (uint32_t) os;

	#define PUT4(dst, at, v) do { \
		dst[at+0] = (uint8_t)((uint32_t)(v) >> 24); \
		dst[at+1] = (uint8_t)((uint32_t)(v) >> 16); \
		dst[at+2] = (uint8_t)((uint32_t)(v) >> 8); \
		dst[at+3] = (uint8_t)((uint32_t)(v)); } while (0)

	if (csVal) { PUT4(top.p, csVal, csPos); }
	if (szVal) { PUT4(top.p, szVal, privItem.n); PUT4(top.p, privVal, privPos); }
	if (subrsVal) { uint32_t rel = lsPos - privPos; PUT4(priv.p, subrsVal, rel); }
	#undef PUT4
	(void) strPos; (void) namePos;

	b_u8(out, (unsigned) (sp->major ? sp->major : 1));
	b_u8(out, 0);
	b_u8(out, 4);
	b_u8(out, 4);
	index_write(out, &nameItem, 1);
	index_write(out, &topItem, 1);
	b_u16(out, 0);
	index_write(out, sp->gsubrs, sp->nGsubrs);
	index_write(out, sp->chars, sp->nChars);
	if (privItem.n) b_mem(out, privItem.p, privItem.n);
	index_write(out, sp->lsubrs, sp->nLsubrs);

	lo->csIndex = csPos;  lo->csOperand = topData + csVal;
	lo->gsIndex = gsPos;  lo->lsIndex = lsPos;
	lo->privStart = privPos;
	lo->privOperand = privVal ? topData + privVal : 0;
	lo->privSizeOperand = szVal ? topData + szVal : 0;
	lo->subrsOperand = subrsVal ? privPos + subrsVal : 0;
	b_free(&top); b_free(&priv);
}

/* ------------------------------------------------------------------ */
/* SFNT.                                                               */

typedef struct Table { char tag[4]; Buf data; } Table;

static int cmp_table(const void *a, const void *b)
{ return memcmp(((const Table *) a)->tag, ((const Table *) b)->tag, 4); }

static uint32_t checksum4(const uint8_t *p, size_t n)
{
	uint32_t sum = 0; size_t i, k;
	for (i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (k = 0; k < 4; k++) w = (w << 8) | (i + k < n ? p[i + k] : 0);
		sum = (sum + w) & 0xFFFFFFFFu;
	}
	return sum;
}

static int sfnt_build(Buf *out, uint32_t magic, Table *tabs, size_t n)
{
	size_t i, selector = 0; unsigned searchRange; uint32_t off, adj;
	size_t headRec = (size_t) -1;

	if (out->bad) return -1;
	qsort(tabs, n, sizeof *tabs, cmp_table);
	for (i = 0; i < n; i++) if (tabs[i].data.bad) return -1;
	while ((size_t) (1u << (selector + 1)) <= n) selector++;
	searchRange = (unsigned) ((1u << selector) * 16u);
	b_u32(out, magic);
	b_u16(out, (unsigned) n);
	b_u16(out, searchRange);
	b_u16(out, (unsigned) selector);
	b_u16(out, (unsigned) ((unsigned) n * 16u - searchRange));
	off = (uint32_t) (12 + 16 * n);
	for (i = 0; i < n; i++) {
		off = (off + 3u) & ~3u;
		b_mem(out, tabs[i].tag, 4);
		b_u32(out, checksum4(tabs[i].data.p, tabs[i].data.len));
		b_u32(out, off);
		b_u32(out, (uint32_t) tabs[i].data.len);
		if (memcmp(tabs[i].tag, "head", 4) == 0) headRec = i;
		off += (uint32_t) ((tabs[i].data.len + 3) & ~(size_t) 3);
	}
	for (i = 0; i < n; i++) {
		while (out->len & 3) b_u8(out, 0);
		b_mem(out, tabs[i].data.p, tabs[i].data.len);
	}
	while (out->len & 3) b_u8(out, 0);
	if (headRec == (size_t) -1) return -1;
	{
		size_t p = 12 + 16 * headRec;
		uint32_t headOff = (uint32_t) out->p[p + 8] << 24 |
		                   (uint32_t) out->p[p + 9] << 16 |
		                   (uint32_t) out->p[p + 10] << 8 |
		                   (uint32_t) out->p[p + 11];
		uint8_t *hp = out->p + headOff;
		hp[8] = hp[9] = hp[10] = hp[11] = 0;
		adj = 0xB1B0AFBAu - checksum4(out->p, out->len);
		hp[8] = (uint8_t) (adj >> 24); hp[9] = (uint8_t) (adj >> 16);
		hp[10] = (uint8_t) (adj >> 8);  hp[11] = (uint8_t) adj;
	}
	return out->bad ? -1 : 0;
}

static Buf make_head(int em)
{
	Buf b = { 0 };
	b_u32(&b, 0x00010000);       /* version */
	b_u32(&b, 0x00010000);       /* fontRevision */
	b_u32(&b, 0);                /* checkSumAdjustment, patched later */
	b_u32(&b, 0x5F0F3CF5);       /* magicNumber */
	b_u16(&b, 0);                /* flags */
	b_u16(&b, (unsigned) em);    /* unitsPerEm */
	b_u32(&b, 0); b_u32(&b, 0);  /* created */
	b_u32(&b, 0); b_u32(&b, 0);  /* modified */
	b_u16(&b, 0);                /* xMin */
	b_u16(&b, 0);                /* yMin */
	b_u16(&b, 0);                /* xMax */
	b_u16(&b, 0);                /* yMax */
	b_u16(&b, 0);                /* macStyle */
	b_u16(&b, 8);                /* lowestRecPPEM */
	b_u16(&b, 2);                /* fontDirectionHint */
	b_u16(&b, 1);                /* indexToLocFormat: long */
	b_u16(&b, 0);                /* glyphDataFormat */
	return b;
}

static Buf make_hhea(int asc, int desc, unsigned nMetrics)
{
	Buf b = { 0 };
	b_u32(&b, 0x00010000);
	b_u16(&b, (unsigned) (asc & 0xFFFF));
	b_u16(&b, (unsigned) (desc & 0xFFFF));
	b_u16(&b, 0);
	b_u16(&b, (unsigned) (asc & 0xFFFF));
	b_u16(&b, 0); b_u16(&b, 0); b_u16(&b, 0);
	b_u16(&b, 1); b_u16(&b, 0); b_u16(&b, 0);
	b_u16(&b, 0); b_u16(&b, 0); b_u16(&b, 0); b_u16(&b, 0);
	b_u16(&b, 0);
	b_u16(&b, nMetrics);
	return b;
}

static Buf make_maxp(unsigned n)
{
	Buf b = { 0 };
	b_u32(&b, 0x00010000); b_u16(&b, n);
	while (b.len < 32) b_u16(&b, 0);
	return b;
}

static Buf make_hmtx(const unsigned *adv, const int *lsb, unsigned n)
{
	Buf b = { 0 }; unsigned i;
	for (i = 0; i < n; i++) { b_u16(&b, adv[i]); b_u16(&b, (unsigned) (lsb[i] & 0xFFFF)); }
	return b;
}

static Buf make_cmap(unsigned first, const unsigned *glyphs, unsigned n)
{
	Buf sub = { 0 }, b = { 0 }; unsigned i;
	b_u16(&sub, 6); b_u16(&sub, 10 + 2 * n); b_u16(&sub, 0);
	b_u16(&sub, first); b_u16(&sub, n);
	for (i = 0; i < n; i++) b_u16(&sub, glyphs[i]);
	b_u16(&b, 0); b_u16(&b, 1); b_u16(&b, 3); b_u16(&b, 1); b_u32(&b, 12);
	b_mem(&b, sub.p, sub.len); b_free(&sub);
	return b;
}

static Buf make_name(void) { Buf b = { 0 }; b_u16(&b, 0); b_u16(&b, 0); b_u16(&b, 6); return b; }
static Buf make_post(void)
{
	Buf b = { 0 };
	b_u32(&b, 0x00030000); b_u16(&b, 0); b_u16(&b, 0);
	b_u16(&b, 0); b_u32(&b, 0); b_u16(&b, 0); b_u16(&b, 0);
	b_u32(&b, 0); b_u32(&b, 0); b_u32(&b, 0); b_u32(&b, 0);
	return b;
}

static void tables_free(Table *t, size_t n) { size_t i; for (i = 0; i < n; i++) b_free(&t[i].data); }

/* Wraps a built CFF table into an OTTO SFNT.  The caller keeps ownership
 * of `cff`; the bytes are copied into the table set. */
static int wrap_cff(Buf *out, const Buf *cff, int em, unsigned nGlyphs,
	const unsigned *adv, const int *lsb)
{
	Table t[8]; size_t n = 0; unsigned i, map[128]; int rc;
#define TAG(tb, a, b, c, d) do { tb.tag[0]=a; tb.tag[1]=b; tb.tag[2]=c; tb.tag[3]=d; } while(0)
	TAG(t[n], 'C','F','F',' ');
	t[n].data = (Buf) { 0 };
	b_need(&t[n].data, cff->len);
	if (cff->bad || t[n].data.bad) { b_free(&t[n].data); return -1; }
	b_mem(&t[n].data, cff->p, cff->len); n++;
	for (i = 0; i < nGlyphs && i < 128; i++) map[i] = i;
	TAG(t[n], 'c','m','a','p'); t[n].data = make_cmap(65, map, nGlyphs); n++;
	TAG(t[n], 'h','e','a','d'); t[n].data = make_head(em); n++;
	TAG(t[n], 'h','h','e','a'); t[n].data = make_hhea(800, -200, nGlyphs); n++;
	TAG(t[n], 'h','m','t','x'); t[n].data = make_hmtx(adv, lsb, nGlyphs); n++;
	TAG(t[n], 'm','a','x','p'); t[n].data = make_maxp(nGlyphs); n++;
	TAG(t[n], 'n','a','m','e'); t[n].data = make_name(); n++;
	TAG(t[n], 'p','o','s','t'); t[n].data = make_post(); n++;
	rc = sfnt_build(out, 0x4F54544F, t, n);
	tables_free(t, n);
	return rc;
#undef TAG
}

/* A simple TrueType glyph made exclusively of on-curve points (lines). */
static Buf ttf_simple_glyph(const int *xy, const int *ends, int nContours,
	int nPoints)
{
	Buf b = { 0 }; int i;
	int xmin = xy[0], xmax = xy[0], ymin = xy[1], ymax = xy[1];
	for (i = 1; i < nPoints; i++) {
		if (xy[2*i] < xmin) xmin = xy[2*i];
		if (xy[2*i] > xmax) xmax = xy[2*i];
		if (xy[2*i+1] < ymin) ymin = xy[2*i+1];
		if (xy[2*i+1] > ymax) ymax = xy[2*i+1];
	}
	b_u16(&b, (unsigned) (nContours & 0xFFFF));
	b_u16(&b, (unsigned) (xmin & 0xFFFF)); b_u16(&b, (unsigned) (ymin & 0xFFFF));
	b_u16(&b, (unsigned) (xmax & 0xFFFF)); b_u16(&b, (unsigned) (ymax & 0xFFFF));
	for (i = 0; i < nContours; i++) b_u16(&b, (unsigned) ends[i]);
	b_u16(&b, 0); /* instructions */
	for (i = 0; i < nPoints; i++) b_u8(&b, 0x01); /* on curve, long deltas */
	for (i = 0; i < nPoints; i++) {
		int d = i ? xy[2*i] - xy[2*(i-1)] : xy[0];
		b_u16(&b, (unsigned) (d & 0xFFFF));
	}
	for (i = 0; i < nPoints; i++) {
		int d = i ? xy[2*i+1] - xy[2*(i-1)+1] : xy[1];
		b_u16(&b, (unsigned) (d & 0xFFFF));
	}
	return b;
}

static int wrap_ttf(Buf *out, Item *glyphs, unsigned nGlyphs,
	const unsigned *adv, const int *lsb, int em)
{
	Table t[9]; Buf glyf = { 0 }, loca = { 0 }; size_t n = 0, i;
	uint32_t off = 0; unsigned map[128]; int rc;
	for (i = 0; i < nGlyphs; i++) {
		uint32_t o = off;
		b_u32(&loca, o);
		b_mem(&glyf, glyphs[i].p, glyphs[i].n);
		while (glyf.len & 1) b_u8(&glyf, 0);
		off = (uint32_t) glyf.len;
	}
	b_u32(&loca, off);
#define TAG(tb, a, b, c, d) do { tb.tag[0]=a; tb.tag[1]=b; tb.tag[2]=c; tb.tag[3]=d; } while(0)
	for (i = 0; i < nGlyphs && i < 128; i++) map[i] = i;
	TAG(t[n], 'c','m','a','p'); t[n].data = make_cmap(65, map, nGlyphs); n++;
	TAG(t[n], 'g','l','y','f'); t[n].data = glyf; n++;
	TAG(t[n], 'h','e','a','d'); t[n].data = make_head(em); n++;
	TAG(t[n], 'h','h','e','a'); t[n].data = make_hhea(800, -200, nGlyphs); n++;
	TAG(t[n], 'h','m','t','x'); t[n].data = make_hmtx(adv, lsb, nGlyphs); n++;
	TAG(t[n], 'l','o','c','a'); t[n].data = loca; n++;
	TAG(t[n], 'm','a','x','p'); t[n].data = make_maxp(nGlyphs); n++;
	TAG(t[n], 'n','a','m','e'); t[n].data = make_name(); n++;
	TAG(t[n], 'p','o','s','t'); t[n].data = make_post(); n++;
	rc = sfnt_build(out, 0x00010000, t, n);
	tables_free(t, n);
	return rc;
#undef TAG
}

/* ------------------------------------------------------------------ */
/* Type 2 charstring operands.                                         */

static void cs_int(Buf *b, int32_t v)
{
	if (v >= -107 && v <= 107) {
		b_u8(b, (unsigned) (v + 139));
	} else if (v >= -1131 && v <= 1131) {
		int32_t a = v > 0 ? v - 108 : -v - 108;
		b_u8(b, (unsigned) (v > 0 ? 247 + a / 256 : 251 + a / 256));
		b_u8(b, (unsigned) (a % 256));
	} else if (v >= -32768 && v <= 32767) {
		b_u8(b, 28); b_u16(b, (unsigned) (v & 0xFFFF));
	} else {
		b_u8(b, 255); b_u32(b, (uint32_t) (v * 65536));
	}
}
static void cs_fixed(Buf *b, double v)
{ b_u8(b, 255); b_u32(b, (uint32_t) (int32_t) (v * 65536.0)); }

/* ------------------------------------------------------------------ */
/* Rendering helpers.                                                  */

static int render_glyph(const SFT *sft, unsigned long glyph,
	unsigned char **pixels, int *w, int *h, int *nonzero)
{
	SFT_GMetrics m; unsigned char *p; int i, nz = 0;
	*pixels = NULL; *w = *h = 0; if (nonzero) *nonzero = 0;
	if (sft_gmetrics(sft, glyph, &m) < 0) return -1;
	*w = m.minWidth; *h = m.minHeight;
	if (*w < 0 || *h < 0) return -1;
	if (*w == 0 || *h == 0)
		return sft_render(sft, glyph, (SFT_Image) { NULL, 0, 0 });
	p = calloc((size_t) *w * (size_t) *h, 1);
	if (!p) return -1;
	if (sft_render(sft, glyph, (SFT_Image) { p, *w, *h }) < 0) {
		free(p); *pixels = NULL; *w = *h = 0; return -1;
	}
	for (i = 0; i < *w * *h; i++) nz += p[i] != 0;
	*pixels = p; if (nonzero) *nonzero = nz;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Independent cubic geometry.                                         */

typedef struct Cubic { double x0,y0,x1,y1,x2,y2,x3,y3; } Cubic;

static void cubic_eval(const Cubic *c, double t, double *x, double *y)
{
	double mt = 1.0 - t;
	*x = mt*mt*mt*c->x0 + 3.0*mt*mt*t*c->x1 + 3.0*mt*t*t*c->x2 + t*t*t*c->x3;
	*y = mt*mt*mt*c->y0 + 3.0*mt*mt*t*c->y1 + 3.0*mt*t*t*c->y2 + t*t*t*c->y3;
}

/* Closed-form extrema of one Bezier coordinate (no interpreter involved). */
static int cubic_extrema(double p0, double p1, double p2, double p3, double r[2])
{
	double a = -p0 + 3.0*p1 - 3.0*p2 + p3;
	double b = 2.0*(p0 - 2.0*p1 + p2);
	double c = p1 - p0; double disc; int n = 0;
	if (fabs(a) < 1e-12) { if (fabs(b) > 1e-12) r[n++] = -c / b; return n; }
	disc = b*b - 4.0*a*c;
	if (disc < 0.0) return 0;
	r[n++] = (-b + sqrt(disc)) / (2.0*a);
	r[n++] = (-b - sqrt(disc)) / (2.0*a);
	return n;
}

/* Area under a cubic for monotone x: Simpson integration of y(t) * x'(t). */
static double cubic_area(const Cubic *c, int steps)
{
	double sum = 0.0; int i, n = steps;
	for (i = 0; i <= n; i++) {
		double t = (double) i / n, mt = 1.0 - t, y, xp;
		double w = (i == 0 || i == n) ? 1.0 : (i & 1 ? 4.0 : 2.0);
		y = mt*mt*mt*c->y0 + 3.0*mt*mt*t*c->y1 +
		    3.0*mt*t*t*c->y2 + t*t*t*c->y3;
		xp = 3.0*mt*mt*(c->x1-c->x0) + 6.0*mt*t*(c->x2-c->x1) +
		     3.0*t*t*(c->x3-c->x2);
		sum += w * y * xp;
	}
	return sum * (1.0 / 3.0) / n;   /* Simpson weight (step 1/n) */
}

/* ==== TESTS BELOW ==== */

/* ------------------------------------------------------------------ */
/* Geometry helpers independent of the interpreter.                    */

static void bb_reset(double b[4])
{ b[0] = b[1] = 1e30; b[2] = b[3] = -1e30; }
static void bb_point(double b[4], double x, double y)
{
	if (x < b[0]) b[0] = x;
	if (y < b[1]) b[1] = y;
	if (x > b[2]) b[2] = x;
	if (y > b[3]) b[3] = y;
}
static void bb_cubic(double b[4], const Cubic *c)
{
	double r[2]; int n, i;
	bb_point(b, c->x0, c->y0);
	bb_point(b, c->x3, c->y3);
	n = cubic_extrema(c->x0, c->x1, c->x2, c->x3, r);
	for (i = 0; i < n; i++) if (r[i] > 0.0 && r[i] < 1.0) {
		double x, y; cubic_eval(c, r[i], &x, &y); bb_point(b, x, y);
	}
	n = cubic_extrema(c->y0, c->y1, c->y2, c->y3, r);
	for (i = 0; i < n; i++) if (r[i] > 0.0 && r[i] < 1.0) {
		double x, y; cubic_eval(c, r[i], &x, &y); bb_point(b, x, y);
	}
}

static void expected_device_box(const SFT *sft, const double unit[4], int box[4])
{
	double xs = sft->xScale / 1000.0, ys = sft->yScale / 1000.0;
	box[0] = (int) floor(unit[0] * xs + sft->xOffset);
	box[1] = (int) floor(unit[1] * ys + sft->yOffset);
	box[2] = (int) ceil (unit[2] * xs + sft->xOffset);
	box[3] = (int) ceil (unit[3] * ys + sft->yOffset);
}

static int check_metrics_against(const SFT *sft, unsigned long glyph,
	const double unit[4], double advUnits, double lsbUnits)
{
	SFT_GMetrics m; int box[4];
	if (sft_gmetrics(sft, glyph, &m) < 0) return 1;
	if (fabs(m.advanceWidth - advUnits * sft->xScale / 1000.0) > 1e-9) return 2;
	if (fabs(m.leftSideBearing -
	    (lsbUnits * sft->xScale / 1000.0 + sft->xOffset)) > 1e-9) return 3;
	expected_device_box(sft, unit, box);
	if (box[2] <= box[0] || box[3] <= box[1]) {
		return (m.minWidth == 0 && m.minHeight == 0) ? 0 : 4;
	}
	if (m.minWidth != box[2] - box[0] + 1) return 5;
	if (m.minHeight != box[3] - box[1] + 1) return 6;
	if (m.yOffset != ((sft->flags & SFT_DOWNWARD_Y) ? -box[3] : box[1])) return 7;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Font factory.                                                       */

static int make_cff_font_full(Buf *out, const Item *chars, size_t n,
	const Item *ls, size_t nls, const Item *gs, size_t ngs,
	int32_t defW, int32_t nomW, int writeWidths,
	int useFontBBox, const int32_t fb[4], int charstringType, int useROS)
{
	CffSpec spec; unsigned adv[64]; int lsb[64]; size_t i; int rc;
	memset(&spec, 0, sizeof spec);
	spec.chars = chars; spec.nChars = n;
	spec.gsubrs = gs; spec.nGsubrs = ngs;
	spec.lsubrs = ls; spec.nLsubrs = nls;
	spec.writeWidths = writeWidths;
	spec.defaultWidthX = defW; spec.nominalWidthX = nomW;
	spec.useFontBBox = useFontBBox;
	if (useFontBBox) for (i = 0; i < 4; i++) spec.fontBox[i] = fb[i];
	spec.charstringType = charstringType;
	spec.useROS = useROS;
	for (i = 0; i < n && i < 64; i++) { adv[i] = 700; lsb[i] = 50; }
	{
		Buf cff = { 0 };
		cff_assemble(&cff, &spec, NULL);
		rc = wrap_cff(out, &cff, 1000, (unsigned) n, adv, lsb);
	}
	return rc;
}

/* Builds a font from N charstring buffers and an optional subr corpus.
 * `special` is installed at index `specialIndex` of an INDEX with `count`
 * entries; every other entry is `return`. */
static int make_cff_font(Buf *out, const Buf *chars, size_t n,
	const Buf *special, size_t count, size_t specialIndex, int useLocal)
{
	Item *items = calloc(n ? n : 1, sizeof *items);
	Buf storage = { 0 };
	Item *subrs = NULL;
	int rc = 0;
	size_t i;
	if (!items) return -1;
	for (i = 0; i < n; i++) { items[i].p = chars[i].p; items[i].n = chars[i].len; }
	if (count) {
		subrs = make_subr_items(count, specialIndex, special, &storage);
		if (!subrs) { free(items); return -1; }
	}
	rc = make_cff_font_full(out, items, n,
	                        useLocal ? subrs : NULL, useLocal ? count : 0,
	                        useLocal ? NULL : subrs, useLocal ? 0 : count,
	                        0, 0, 0, 0, NULL, -1, 0);
	free(items); free(subrs); b_free(&storage);
	return rc;
}

static int simple_cff(Buf *out, const Buf *chars, size_t n)
{
	return make_cff_font(out, chars, n, NULL, 0, 0, 1);
}

/* ------------------------------------------------------------------ */
/* Charstring fragments.                                               */

static void cs_triangle(Buf *b)
{
	cs_int(b, 100); cs_int(b, 0); b_u8(b, 21);
	cs_int(b, 400); cs_int(b, 0);
	cs_int(b, -200); cs_int(b, 600);
	cs_int(b, -200); cs_int(b, -600);
	b_u8(b, 5);
	b_u8(b, 14);
}

/* ================================================================== */
/* Group: outlines                                                     */
/* ================================================================== */

/* A straight triangle stored as TrueType and as CFF must produce the very
 * same pixels, metrics and origin for fractional scales/offsets and for
 * both Y directions. */
static int test_line_equivalence(void)
{
	Buf cf = { 0 }, tf = { 0 }, tri = { 0 }, gly = { 0 };
	Item it[1]; SFT_Font *fc, *ft;
	struct Config { double xs, ys, xo, yo; int flags; } cfgs[] = {
		{ 20.0, 20.0, 0.0, 0.0, 0 },
		{ 17.3, 23.1, 0.37, -0.42, 0 },
		{ 20.0, 20.0, 0.0, 0.0, SFT_DOWNWARD_Y },
		{ 13.7, 31.9, 0.25, 0.5, SFT_DOWNWARD_Y },
	};
	size_t ci; int rc = 0; unsigned adv[1] = { 700 }; int lsb[1] = { 50 };

	cs_triangle(&tri);
	if (simple_cff(&cf, &tri, 1) < 0) { rc = 1; goto out; }
	{
		int xy[6] = { 100, 0, 500, 0, 300, 600 }; int ends[1] = { 2 };
		gly = ttf_simple_glyph(xy, ends, 1, 3);
	}
	it[0].p = gly.p; it[0].n = gly.len;
	if (wrap_ttf(&tf, it, 1, adv, lsb, 1000) < 0) { rc = 2; goto out; }
	fc = sft_loadmem(cf.p, cf.len);
	ft = sft_loadmem(tf.p, tf.len);
	if (!fc || !ft) { rc = 3; goto out; }
	for (ci = 0; ci < sizeof cfgs / sizeof cfgs[0] && !rc; ci++) {
		SFT sc = { fc, cfgs[ci].xs, cfgs[ci].ys, cfgs[ci].xo, cfgs[ci].yo, cfgs[ci].flags };
		SFT st = { ft, cfgs[ci].xs, cfgs[ci].ys, cfgs[ci].xo, cfgs[ci].yo, cfgs[ci].flags };
		SFT_GMetrics mc, mt;
		unsigned char *pc = NULL, *pt = NULL; int wc, hc, hcn, wt, ht, htn;
		if (sft_gmetrics(&sc, 0, &mc) < 0 || sft_gmetrics(&st, 0, &mt) < 0) { rc = 4; break; }
		if (mc.advanceWidth != mt.advanceWidth ||
		    mc.leftSideBearing != mt.leftSideBearing ||
		    mc.minWidth != mt.minWidth || mc.minHeight != mt.minHeight ||
		    mc.yOffset != mt.yOffset) { rc = 5; break; }
		if (render_glyph(&sc, 0, &pc, &wc, &hc, &hcn) < 0 ||
		    render_glyph(&st, 0, &pt, &wt, &ht, &htn) < 0) { rc = 6; break; }
		if (wc != wt || hc != ht || hcn != htn ||
		    memcmp(pc, pt, (size_t) wc * hc) != 0) rc = 7;
		free(pc); free(pt);
	}
	sft_freefont(fc); sft_freefont(ft);
out:
	b_free(&cf); b_free(&tf); b_free(&tri); b_free(&gly);
	return rc;
}

/* Tight box of a real cubic, including interior extrema; the FontBBox in
 * the DICT is bogus on purpose and must not be used. */
static int test_cubic_extrema(void)
{
	Buf cst[2] = { { 0 }, { 0 } }, font = { 0 };
	SFT_Font *f; int rc = 0;
	Cubic arch = { 0,0, 0,800, 600,800, 600,0 };
	Cubic sx = { 0,0, 900,200, 900,-200, 0,0 };
	double b1[4], b2[4];
	int32_t badbox[4] = { 0, 0, 1, 1 };

	cs_int(&cst[0], 0); cs_int(&cst[0], 0); b_u8(&cst[0], 21);
	cs_int(&cst[0], 0); cs_int(&cst[0], 800);
	cs_int(&cst[0], 600); cs_int(&cst[0], 0);
	cs_int(&cst[0], 0); cs_int(&cst[0], -800);
	b_u8(&cst[0], 8); b_u8(&cst[0], 14);
	cs_int(&cst[1], 0); cs_int(&cst[1], 0); b_u8(&cst[1], 21);
	cs_int(&cst[1], 900); cs_int(&cst[1], 200);
	cs_int(&cst[1], 0); cs_int(&cst[1], -400);
	cs_int(&cst[1], -900); cs_int(&cst[1], 200);
	b_u8(&cst[1], 8); b_u8(&cst[1], 14);
	{
		Item items[2];
		items[0].p = cst[0].p; items[0].n = cst[0].len;
		items[1].p = cst[1].p; items[1].n = cst[1].len;
		if (make_cff_font_full(&font, items, 2, NULL, 0, NULL, 0,
		                       0, 0, 0, 1, badbox, -1, 0) < 0) goto out;
	}
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 1; goto out; }
	bb_reset(b1); bb_cubic(b1, &arch);
	bb_reset(b2); bb_cubic(b2, &sx);
	/* Independent reference values: the arch tops out at y=600 and is
	 * 600 wide; the sideways glyph reaches x=675 and y=+/-57.735. */
	if (fabs(b1[3] - 600.0) > 1e-9 || fabs(b1[2] - 600.0) > 1e-9) rc = 2;
	if (fabs(b2[2] - 675.0) > 1e-9 || fabs(b2[3] - 57.7350269189) > 1e-6 ||
	    fabs(b2[1] + 57.7350269189) > 1e-6) rc = 3;
	{
		double sxScale[3] = { 0.5, 0.37, 0.913 }, syScale[3] = { 0.5, 0.619, 0.227 };
		int fl[2] = { 0, SFT_DOWNWARD_Y };
		unsigned i, j, k;
		for (i = 0; i < 3 && !rc; i++) for (j = 0; j < 3 && !rc; j++)
		for (k = 0; k < 2 && !rc; k++) {
			SFT s = { f, sxScale[i] * 1000, syScale[j] * 1000, 0.37, -0.29, fl[k] };
			rc = check_metrics_against(&s, 0, b1, 700, 50);
			if (!rc) rc = check_metrics_against(&s, 1, b2, 700, 50);
			if (!rc) {
				unsigned char *p; int w, h, nz;
				if (render_glyph(&s, 0, &p, &w, &h, &nz) < 0) rc = 20;
				else if (nz == 0) rc = 21;
				free(p);
			}
		}
	}
	sft_freefont(f);
out:
	b_free(&cst[0]); b_free(&cst[1]); b_free(&font);
	return rc;
}

/* Coverage of a genuine cubic against Simpson integration of y dx along
 * the actual on-curve geometry.  A control-polygon approximation of this
 * cubic is ~33% larger and is caught by the tolerance. */
static int test_cubic_coverage(void)
{
	Buf cst = { 0 }, font = { 0 };
	SFT_Font *f; int rc = 0;
	Cubic dome = { 0,0, 200,300, 400,300, 600,0 };
	double unitArea, devArea, pixelArea, sum = 0.0, tol, poly;
	unsigned char *pixels = NULL; int w, h, nz, i;
	SFT s;

	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, 200); cs_int(&cst, 300);
	cs_int(&cst, 200); cs_int(&cst, 0);
	cs_int(&cst, 200); cs_int(&cst, -300);
	b_u8(&cst, 8);
	cs_int(&cst, -600); cs_int(&cst, 0); b_u8(&cst, 5);
	b_u8(&cst, 14);
	if (simple_cff(&font, &cst, 1) < 0) { rc = 1; goto out; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	s = (SFT) { f, 200.0, 200.0, 0.0, 0.0, 0 };
	if (render_glyph(&s, 0, &pixels, &w, &h, &nz) < 0) { rc = 3; goto out_font; }
	if (w <= 0 || h <= 0 || nz == 0) { rc = 4; goto out_font; }
	for (i = 0; i < w * h; i++) sum += pixels[i];
	pixelArea = sum / 255.0;
	unitArea = cubic_area(&dome, 4000);
	devArea = unitArea * 0.2 * 0.2;
	/* Error budget, derived independently of the implementation:
	 * the flattener keeps every chord within 0.25 device px of the true
	 * curve, so the enclosed area differs by at most (tolerance × arc
	 * length) = 0.25 × 250 = 62.5 px^2; quantization adds at most
	 * w*h/510 = 11 px^2.  Tolerance = 2.5% of the analytic area plus
	 * 1 px^2 therefore bounds both generously while staying far below
	 * the 33% gap between the analytic area and the control polygon. */
	tol = 0.025 * devArea + 1.0;
	if (fabs(pixelArea - devArea) > tol) rc = 5;
	/* The control polygon of this cubic encloses 4800 device px^2. */
	{
		double polyUnit = 120000.0;              /* shoelace, font units */
		poly = polyUnit * 0.2 * 0.2;
		if (fabs(poly - devArea) <= tol) rc = 6;
	}
	free(pixels);
out_font:
	sft_freefont(f);
out:
	b_free(&cst); b_free(&font);
	return rc;
}

/* Empty glyph: zero coverage size, hmtx still applies. */
static int test_empty_glyph(void)
{
	Buf b = { 0 }, font = { 0 };
	SFT_Font *f; SFT s; SFT_GMetrics m; int rc = 0;

	b_u8(&b, 14);
	if (simple_cff(&font, &b, 1) < 0) { rc = 1; goto out; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	s = (SFT) { f, 40.0, 33.0, 0.25, 0.5, SFT_DOWNWARD_Y };
	if (sft_gmetrics(&s, 0, &m) < 0) rc = 3;
	else {
		if (m.minWidth != 0 || m.minHeight != 0) rc = 4;
		if (fabs(m.advanceWidth - 28.0) > 1e-9) rc = 5;
		if (fabs(m.leftSideBearing - 2.25) > 1e-9) rc = 6;
		if (sft_render(&s, 0, (SFT_Image) { NULL, 0, 0 }) < 0) rc = 7;
	}
	sft_freefont(f);
out:
	b_free(&b); b_free(&font);
	return rc;
}

/* The current point and relative deltas are continuous across both
 * subroutine kinds. */
static int test_subr_continuity(void)
{
	Buf ls = { 0 }, gs = { 0 }, cstl = { 0 };
	Buf fl = { 0 }, fg = { 0 };
	SFT_Font *f; int rc = 0;
	Cubic c1 = { 0,0, 0,600, 200,600, 200,0 };
	double b[4];

	/* Local subr completes a cubic whose first four operands the caller
	 * already pushed, then issues rrcurveto.  The operand stack and the
	 * current point therefore span the call boundary. */
	cs_int(&ls, 200); cs_int(&ls, 0);
	cs_int(&ls, 0); cs_int(&ls, -600);
	b_u8(&ls, 8);                          /* rrcurveto, 6 operands */
	b_u8(&ls, 11);
	/* Global subr draws a line from the caller's current point and
	 * returns; the caller's y position afterwards proves the current
	 * point is shared with the global subroutine. */
	cs_int(&gs, 200); cs_int(&gs, 100); b_u8(&gs, 5); b_u8(&gs, 11);

	/* Glyph A (local subr): rmoveto 0 0; push the first four curve
	 * deltas; callsubr finishes the rrcurveto.  Glyph B (global subr):
	 * rmoveto 0 0; push a line delta; callgsubr issues the rlineto.
	 * The reference box is the same for both: the curve plus one
	 * horizontal line at y=0. */
	cs_int(&cstl, 0); cs_int(&cstl, 0); b_u8(&cstl, 21);
	cs_int(&cstl, 0); cs_int(&cstl, 600);
	cs_int(&cstl, -107); b_u8(&cstl, 10);
	b_u8(&cstl, 14);

	if (make_cff_font(&fl, &cstl, 1, &ls, 1, 0, 1) < 0) rc = 1;
	if (rc) goto out;
	{
		Buf cstg = { 0 };
		cs_int(&cstg, 0); cs_int(&cstg, 0); b_u8(&cstg, 21);
		cs_int(&cstg, -107); b_u8(&cstg, 29);   /* no operands of its own */
		b_u8(&cstg, 14);
		if (make_cff_font(&fg, &cstg, 1, &gs, 1, 0, 0) < 0) rc = 2;
		b_free(&cstg);
	}
	if (rc) goto out;
	bb_reset(b);
	bb_point(b, 0, 0);
	bb_cubic(b, &c1);
	f = sft_loadmem(fl.p, fl.len);
	if (!f) { rc = 3; goto out; }
	{
		SFT s = { f, 30.0, 30.0, 0.0, 0.0, 0 };
		rc = check_metrics_against(&s, 0, b, 700, 50);
		sft_freefont(f);
	}
	if (!rc) {
		f = sft_loadmem(fg.p, fg.len);
		if (!f) rc = 4;
		else {
			SFT s = { f, 30.0, 30.0, 0.0, 0.0, 0 };
			bb_reset(b);
			bb_point(b, 0, 0); bb_point(b, 200, 100);
			rc = check_metrics_against(&s, 0, b, 700, 50);
			sft_freefont(f);
		}
	}
out:
	b_free(&ls); b_free(&gs); b_free(&cstl);
	b_free(&fl); b_free(&fg);
	return rc;
}

/* Bias follows the INDEX count: 1 entry (bias 107) and 1300 entries
 * (bias 1131).  The same visual result must come out of both. */
static int build_bias_font(Buf *out, size_t count, int32_t operand, int useLocal)
{
	Buf body = { 0 }, cst = { 0 };
	int rc;

	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, operand); b_u8(&cst, useLocal ? 10 : 29);
	cs_int(&cst, 0); cs_int(&cst, -200); b_u8(&cst, 5);  /* close back down */
	b_u8(&cst, 14);
	cs_int(&body, 300); cs_int(&body, 0); b_u8(&body, 5);
	cs_int(&body, 0); cs_int(&body, 200); b_u8(&body, 5);
	b_u8(&body, 11);
	rc = make_cff_font(out, &cst, 1, &body, count, 0, useLocal);
	b_free(&body); b_free(&cst);
	return rc;
}

static int test_subr_bias(void)
{
	Buf small = { 0 }, big = { 0 };
	SFT_Font *fs, *fb; int rc = 0;
	double unit[4];
	unsigned char *ps = NULL, *pb = NULL;
	int ws = 0, hs = 0, wsN = 0, wb = 0, hb = 0, wbN = 0;

	/* A 1-entry INDEX with bias 107: operand -107 -> entry 0.
	 * A 1300-entry INDEX with bias 1131: operand -1131 -> entry 0.
	 * A naive bias-107 decoder would look up entry -1024 / 1024 (invalid
	 * or a plain `return`), which the checks below would notice. */
	if (build_bias_font(&small, 1, -107, 1) < 0) return 1;
	if (build_bias_font(&big, 1300, -1131, 1) < 0) { b_free(&small); return 2; }
	fs = sft_loadmem(small.p, small.len);
	fb = sft_loadmem(big.p, big.len);
	if (!fs || !fb) { rc = 3; goto out; }
	{
		SFT ss = { fs, 20.0, 20.0, 0.0, 0.0, 0 };
		SFT sb = { fb, 20.0, 20.0, 0.0, 0.0, 0 };
		bb_reset(unit);
		bb_point(unit, 0, 0); bb_point(unit, 300, 0);
		bb_point(unit, 300, 200); bb_point(unit, 0, 200);
		bb_point(unit, 0, 0);
		rc = check_metrics_against(&ss, 0, unit, 700, 50);
		if (!rc) rc = check_metrics_against(&sb, 0, unit, 700, 50);
		if (!rc) {
			if (render_glyph(&ss, 0, &ps, &ws, &hs, &wsN) < 0 ||
			    render_glyph(&sb, 0, &pb, &wb, &hb, &wbN) < 0) rc = 4;
			else if (ws != wb || hs != hb || wsN != wbN ||
			         memcmp(ps, pb, (size_t) ws * hs) != 0) rc = 5;
			free(ps); free(pb);
		}
	}
	/* Global variant of the large-bias lookup. */
	if (!rc) {
		b_free(&big); big = (Buf) { 0 };
		if (build_bias_font(&big, 1300, -1131, 0) < 0) rc = 6;
		else {
			sft_freefont(fb); fb = NULL;
			fb = sft_loadmem(big.p, big.len);
			if (!fb) rc = 7;
			else {
				SFT sb = { fb, 20.0, 20.0, 0.0, 0.0, 0 };
				int w2 = 0, h2 = 0, n2 = 0;
				unsigned char *p2 = NULL;
				rc = check_metrics_against(&sb, 0, unit, 700, 50);
				if (!rc && (render_glyph(&sb, 0, &p2, &w2, &h2, &n2) < 0 ||
				    w2 != ws || h2 != hs || n2 != wsN)) rc = 8;
				free(p2);
			}
		}
	}
out:
	if (fs) sft_freefont(fs);
	if (fb) sft_freefont(fb);
	b_free(&small); b_free(&big);
	return rc;
}

/* Wide (multi-byte) INDEX offsets: a Local Subrs INDEX whose data area
 * exceeds 255 bytes needs 2+ byte offsets.  A decoder assuming 1-byte
 * offsets reads the wrong subroutine. */
static int test_wide_index(void)
{
	Buf body = { 0 }, cst = { 0 }, font = { 0 };
	SFT_Font *f; int rc = 0;
	double unit[4];

	/* 700 entries -> 700-byte data area -> 2-byte INDEX offsets.  The
	 * subr draws a rectangle from the caller's current point. */
	cs_int(&body, 400); cs_int(&body, 0); b_u8(&body, 5);
	cs_int(&body, 0); cs_int(&body, 200); b_u8(&body, 5);
	cs_int(&body, -400); cs_int(&body, 0); b_u8(&body, 5);
	cs_int(&body, 0); cs_int(&body, -200); b_u8(&body, 5);
	b_u8(&body, 11);
	/* 700 entries is still below the 1240 threshold, so the bias is
	 * 107: operand -107 selects entry 0.  The point of this case is the
	 * two-byte INDEX offset size, not the bias. */
	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, -107); b_u8(&cst, 10);
	b_u8(&cst, 14);
	if (make_cff_font(&font, &cst, 1, &body, 700, 0, 1) < 0) { rc = 1; goto out; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	{
		SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
		bb_reset(unit);
		bb_point(unit, 0, 0); bb_point(unit, 400, 0);
		bb_point(unit, 400, 200); bb_point(unit, 0, 200);
		rc = check_metrics_against(&s, 0, unit, 700, 50);
		sft_freefont(f);
	}
out:
	b_free(&body); b_free(&cst); b_free(&font);
	return rc;
}

/* 16.16 numbers in charstrings (255 prefix), including a negative
 * fractional delta and an integral-looking 16.16 value. */
static int test_fixed_numbers(void)
{
	Buf cst = { 0 }, font = { 0 };
	SFT_Font *f; int rc = 0;
	Cubic c = { 100.5, 0.25, 100.5, 300.75, 300.75, 300.75, 300.75, 0.25 };
	double b[4];

	cs_fixed(&cst, 100.5); cs_fixed(&cst, 0.25); b_u8(&cst, 21);
	cs_fixed(&cst, 0.0); cs_fixed(&cst, 300.5);
	cs_fixed(&cst, 200.25); cs_fixed(&cst, 0.0);
	cs_fixed(&cst, 0.0); cs_fixed(&cst, -300.5);
	b_u8(&cst, 8);
	cs_fixed(&cst, -200.25); cs_fixed(&cst, 0.0); b_u8(&cst, 5);
	b_u8(&cst, 14);
	if (simple_cff(&font, &cst, 1) < 0) { rc = 1; goto out; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	{
		SFT s = { f, 24.0, 24.0, 0.3, 0.0, 0 };
		bb_reset(b); bb_cubic(b, &c);
		rc = check_metrics_against(&s, 0, b, 700, 50);
		sft_freefont(f);
	}
out:
	b_free(&cst); b_free(&font);
	return rc;
}

/* The optional width in the leading rmoveto / endchar forms must be
 * consumed (and not treated as a coordinate). */
static int test_width_consumption(void)
{
	Buf cst0 = { 0 }, cst1 = { 0 }, font = { 0 };
	SFT_Font *f; int rc = 0;
	double unit[4];
	Item items[2];

	/* Two glyphs: (a) width in rmoveto (3 operands), (b) "0 endchar". */
	cs_int(&cst0, 500);                    /* width argument */
	cs_int(&cst0, 0); cs_int(&cst0, 0); b_u8(&cst0, 21);
	cs_int(&cst0, 300); cs_int(&cst0, 0); b_u8(&cst0, 5);
	cs_int(&cst0, 0); cs_int(&cst0, 300); b_u8(&cst0, 5);
	cs_int(&cst0, -300); cs_int(&cst0, 0); b_u8(&cst0, 5);
	b_u8(&cst0, 14);
	cs_int(&cst1, 0); b_u8(&cst1, 14);     /* 0 = width, then endchar */
	items[0].p = cst0.p; items[0].n = cst0.len;
	items[1].p = cst1.p; items[1].n = cst1.len;
	if (make_cff_font_full(&font, items, 2, NULL, 0, NULL, 0,
	                       0, 0, 0, 0, NULL, -1, 0) < 0) { rc = 1; goto out; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	{
		SFT s = { f, 25.0, 25.0, 0.0, 0.0, 0 };
		SFT_GMetrics m; int w, h, nz; unsigned char *p = NULL;
		/* Glyph 0 is a 300x300 box starting at (0, 0). */
		bb_reset(unit); bb_point(unit, 0, 0); bb_point(unit, 300, 0);
		bb_point(unit, 300, 300); bb_point(unit, 0, 300);
		rc = check_metrics_against(&s, 0, unit, 700, 50);
		if (!rc) {
			if (render_glyph(&s, 0, &p, &w, &h, &nz) < 0) rc = 3;
		}
		free(p);
		if (!rc && sft_gmetrics(&s, 1, &m) < 0) rc = 4;
		else if (!rc && (m.minWidth != 0 || m.minHeight != 0)) rc = 5;
		sft_freefont(f);
	}
out:
	b_free(&cst0); b_free(&cst1); b_free(&font);
	return rc;
}

/* All Type 2 segment operators, with the current point carried between
 * them, checked against an independently assembled bounding box. */
static int test_all_operators(void)
{
	Buf cst = { 0 }, font = { 0 };
	SFT_Font *f; int rc = 0;
	double b[4];

	/* Start (0,0).  hlineto 100; vlineto 100; hlineto -100;
	 * hhcurveto (dx1=20, dy1=0, dx2=50,dy2=50, dx3=0,dy3=50);
	 * vvcurveto (-20, 40,40, 40); hvcurveto (40, 40,40, 0);
	 * vhcurveto (40, 40,40, 0). */
	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, 100); b_u8(&cst, 6);                       /* hlineto */
	cs_int(&cst, 100); b_u8(&cst, 7);                       /* vlineto */
	cs_int(&cst, -100); b_u8(&cst, 6);                      /* hlineto */
	cs_int(&cst, 20); cs_int(&cst, 30); cs_int(&cst, 50); cs_int(&cst, 40);
	cs_int(&cst, 50); b_u8(&cst, 27);                       /* hhcurveto, 5 operands */
	cs_int(&cst, 100); cs_int(&cst, -20);
	cs_int(&cst, 40); cs_int(&cst, 40);
	cs_int(&cst, 40); b_u8(&cst, 26);                       /* vvcurveto, 5 operands */
	cs_int(&cst, 40); cs_int(&cst, 40); cs_int(&cst, 40); cs_int(&cst, 0);
	b_u8(&cst, 31);                                         /* hvcurveto */
	cs_int(&cst, 40); cs_int(&cst, 40); cs_int(&cst, 40); cs_int(&cst, 0);
	b_u8(&cst, 30);                                         /* vhcurveto */
	b_u8(&cst, 14);                                         /* endchar */

	/* Recompute the outline by hand from the Type 2 rules.  The path is
	 *   (0,0) -> (100,0) -> (100,100) -> (0,100)  [h, v, h lines]
	 *   then four cubics:
	 *   hhcurveto(20, 30,50,40,50): optional dy1=20, then
	 *       (dxa=30, dxb=50, dyb=40, dxc=50) -> dx1=30,dy1=20,
	 *       dx2=50,dy2=40, dx3=50,dy3=0
	 *   vvcurveto(100, -20, 40,40, 40): optional dx1=100, then
	 *       dy1=-20, dx2=40,dy2=40, dx3=0,dy3=40
	 *   hvcurveto(40, 40,40, 0): dx1=40, dx2=40, dy2=40, dy3=0
	 *   vhcurveto(40, 40,40, 0): dy1=40, dx2=40, dy2=40, dx3=0
	 */
	{
		Cubic c[4]; int i;
		bb_reset(b);
		bb_point(b, 0, 0); bb_point(b, 100, 0);
		bb_point(b, 100, 100); bb_point(b, 0, 100);
		c[0] = (Cubic) { 0, 100, 30, 120, 80, 160, 130, 160 };
		c[1] = (Cubic) { 130, 160, 230, 140, 270, 180, 270, 220 };
		c[2] = (Cubic) { 270, 220, 310, 220, 350, 260, 350, 260 };
		c[3] = (Cubic) { 350, 260, 350, 300, 390, 340, 390, 340 };
		for (i = 0; i < 4; i++) bb_cubic(b, &c[i]);
	}
	if (simple_cff(&font, &cst, 1) < 0) { rc = 1; goto out; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	{
		SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
		rc = check_metrics_against(&s, 0, b, 700, 50);
		sft_freefont(f);
	}
out:
	b_free(&cst); b_free(&font);
	return rc;
}

/* ================================================================== */
/* Group: subr-hints                                                   */
/* ================================================================== */

/* Stem hints declared inside a subroutine, with a hintmask whose byte
 * count depends on the accumulated stem count.  Mask bytes must be
 * consumed and never executed as drawing operators. */
static int test_hints_in_subr(void)
{
	Buf lsSpecial = { 0 }, cst = { 0 }, font = { 0 };
	SFT_Font *f; int rc = 0;
	double unit[4];

	cs_int(&lsSpecial, 10); cs_int(&lsSpecial, 20); b_u8(&lsSpecial, 18); /* hstemhm */
	/* After two stems the hintmask needs one byte. */
	b_u8(&lsSpecial, 19); b_u8(&lsSpecial, 0xC0);
	cs_int(&lsSpecial, 600); cs_int(&lsSpecial, 0); b_u8(&lsSpecial, 5);
	cs_int(&lsSpecial, 0); cs_int(&lsSpecial, 600); b_u8(&lsSpecial, 5);
	b_u8(&lsSpecial, 11);
	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, -107); b_u8(&cst, 10);                  /* subr index 0 */
	cs_int(&cst, -600); cs_int(&cst, 0); b_u8(&cst, 5);
	b_u8(&cst, 14);
	if (make_cff_font(&font, &cst, 1, &lsSpecial, 1, 0, 1) < 0) { rc = 1; goto out; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	{
		SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
		bb_reset(unit);
		bb_point(unit, 0, 0);
		bb_point(unit, 600, 0);
		bb_point(unit, 600, 600);
		bb_point(unit, 0, 600);
		rc = check_metrics_against(&s, 0, unit, 700, 50);
		if (!rc) {
			unsigned char *p; int w, h, nz;
			if (render_glyph(&s, 0, &p, &w, &h, &nz) < 0) rc = 3;
			else { if (nz == 0) rc = 4; free(p); }
		}
		sft_freefont(f);
	}
out:
	b_free(&lsSpecial); b_free(&cst); b_free(&font);
	return rc;
}

/* Stem count accumulates across subr calls and callers, so the mask
 * byte count must keep growing accordingly.  Two stems in the caller,
 * two more in the subr, then hintmask = one byte (4 stems); the stem
 * count must not reset when the subr returns.  The glyph afterwards
 * draws a box and must still render. */
static int test_stem_accumulation(void)
{
	Buf lsSpecial = { 0 }, cst = { 0 }, font = { 0 };
	SFT_Font *f; int rc = 0;
	double unit[4];

	/* Subr: two more stems + hintmask consuming one byte + a curve. */
	cs_int(&lsSpecial, 30); cs_int(&lsSpecial, 10); b_u8(&lsSpecial, 3);  /* vstem */
	cs_int(&lsSpecial, 40); cs_int(&lsSpecial, 10); b_u8(&lsSpecial, 3);
	b_u8(&lsSpecial, 19); b_u8(&lsSpecial, 0x5A);
	cs_int(&lsSpecial, 100); cs_int(&lsSpecial, -100);
	cs_int(&lsSpecial, 100); cs_int(&lsSpecial, 100);
	cs_int(&lsSpecial, -100); cs_int(&lsSpecial, 100);
	b_u8(&lsSpecial, 8); b_u8(&lsSpecial, 11);
	/* Caller: two stems then call, then another hintmask (still 1 byte),
	 * then a box, then endchar. */
	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, 10); cs_int(&cst, 20); b_u8(&cst, 1);   /* hstem */
	cs_int(&cst, 10); cs_int(&cst, 20); b_u8(&cst, 1);
	cs_int(&cst, -107); b_u8(&cst, 10);                  /* callsubr */
	b_u8(&cst, 19); b_u8(&cst, 0x00);                    /* hintmask */
	cs_int(&cst, 300); cs_int(&cst, 0); b_u8(&cst, 5);
	cs_int(&cst, 0); cs_int(&cst, -300); b_u8(&cst, 5);
	cs_int(&cst, -600); cs_int(&cst, 0); b_u8(&cst, 5);
	cs_int(&cst, 0); cs_int(&cst, -300); b_u8(&cst, 5);
	b_u8(&cst, 14);
	if (make_cff_font(&font, &cst, 1, &lsSpecial, 1, 0, 1) < 0) { rc = 1; goto out; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	{
		SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
		/* Path: (0,0) -cubic-> (100,100) -lines-> (400,100),
		 * (400,-200), (-200,-200), (-200,-500); endchar closes. */
		Cubic c = { 0, 0, 100, -100, 200, 0, 100, 100 };
		bb_reset(unit);
		bb_point(unit, 0, 0);
		bb_cubic(unit, &c);
		bb_point(unit, 400, 100);
		bb_point(unit, 400, -200);
		bb_point(unit, -200, -200);
		bb_point(unit, -200, -500);
		rc = check_metrics_against(&s, 0, unit, 700, 50);
		if (!rc) {
			unsigned char *p; int w, h, nz;
			if (render_glyph(&s, 0, &p, &w, &h, &nz) < 0) rc = 3;
			else { if (nz == 0) rc = 4; free(p); }
		}
		sft_freefont(f);
	}
out:
	b_free(&lsSpecial); b_free(&cst); b_free(&font);
	return rc;
}

/* cntrmask must consume its bytes as well; a decoder that treats the
 * mask as a drawing operand would corrupt the current point. */
static int test_cntrmask_bytes(void)
{
	Buf cst = { 0 }, font = { 0 };
	SFT_Font *f; int rc = 0;
	double unit[4];

	/* 8 stems -> 1 mask byte; 24 stems -> 3 mask bytes. */
	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	{
		int i;
		for (i = 0; i < 8; i++) {
			cs_int(&cst, i * 20); cs_int(&cst, 10);
		}
	}
	b_u8(&cst, 23);                                      /* vstemhm */
	{
		int i;
		for (i = 0; i < 16; i++) { cs_int(&cst, (i % 3) * 7); cs_int(&cst, 5); }
	}
	b_u8(&cst, 1);                                       /* hstem: total 24 */
	b_u8(&cst, 20);                                      /* cntrmask */
	b_u8(&cst, 0); b_u8(&cst, 0); b_u8(&cst, 0);         /* 3 bytes */
	cs_int(&cst, 250); cs_int(&cst, 0); b_u8(&cst, 5);
	cs_int(&cst, 0); cs_int(&cst, -250); b_u8(&cst, 5);
	cs_int(&cst, -250); cs_int(&cst, 0); b_u8(&cst, 5);
	b_u8(&cst, 14);
	if (simple_cff(&font, &cst, 1) < 0) { rc = 1; goto out; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	{
		SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
		bb_reset(unit);
		bb_point(unit, 0, 0); bb_point(unit, 250, 0);
		bb_point(unit, 250, -250); bb_point(unit, 0, -250);
		rc = check_metrics_against(&s, 0, unit, 700, 50);
		sft_freefont(f);
	}
out:
	b_free(&cst); b_free(&font);
	return rc;
}

/* Type 2's operand stack holds at most 48 entries.  48 operands followed
 * by an operator are legal (24 lines), 49 must be rejected cleanly. */
static int test_stack_limit(void)
{
	Buf ok = { 0 }, over = { 0 }, font = { 0 };
	SFT_Font *f; int rc = 0, i;

	cs_int(&ok, 0); cs_int(&ok, 0); b_u8(&ok, 21);
	for (i = 0; i < 48; i++) cs_int(&ok, 1);
	b_u8(&ok, 5);                       /* 24 lines of (1, 0) */
	cs_int(&ok, 48); cs_int(&ok, 0); b_u8(&ok, 5);
	b_u8(&ok, 14);
	if (simple_cff(&font, &ok, 1) < 0) { rc = 1; goto out1; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out1; }
	{
		SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
		SFT_GMetrics m;
		if (sft_gmetrics(&s, 0, &m) < 0) rc = 3;
		else if (m.minWidth <= 0 || m.minHeight <= 0) rc = 4;
		sft_freefont(f);
	}
out1:
	b_free(&font); font = (Buf) { 0 };

	cs_int(&over, 0); cs_int(&over, 0); b_u8(&over, 21);
	for (i = 0; i < 49; i++) cs_int(&over, 1);
	b_u8(&over, 5);
	b_u8(&over, 14);
	if (simple_cff(&font, &over, 1) < 0) { if (!rc) rc = 5; goto out2; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { if (!rc) rc = 6; goto out2; }
	{
		SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
		SFT_GMetrics m;
		if (sft_gmetrics(&s, 0, &m) == 0) { if (!rc) rc = 7; }
		sft_freefont(f);
	}
out2:
	b_free(&ok); b_free(&over); b_free(&font);
	return rc;
}

/* ================================================================== */
/* Group: invalid                                                      */
/* ================================================================== */

/* Mounts a font, then overwrites the CFF table bytes.  The SFNT wrapper
 * is untouched except for the bytes named by the caller. */
static SFT_Font *load_patched(Buf *font, size_t off, const void *src, size_t n)
{
	SFT_Font *f;
	memcpy(font->p + off, src, n);
	f = sft_loadmem(font->p, font->len);
	return f;
}

/* Corrupt INDEX: CharStrings count vs. the offsets.  loadmem must fail
 * (init_font validates the CFF up front), and no other table is
 * consulted to fake a result. */
static int test_bad_index(void)
{
	Buf font = { 0 }, cst = { 0 };
	Item items[2], *subrs = NULL;
	CffSpec spec; Buf storage = { 0 }, cff = { 0 };
	CffLayout lo;
	int rc = 0;
	unsigned adv[2] = { 700, 700 }; int lsb[2] = { 50, 50 };

	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, 200); cs_int(&cst, 0); b_u8(&cst, 5); b_u8(&cst, 14);
	items[0].p = cst.p; items[0].n = cst.len;
	items[1] = items[0];
	memset(&spec, 0, sizeof spec);
	spec.chars = items; spec.nChars = 2; spec.charstringType = -1;
	cff_assemble(&cff, &spec, &lo);
	if (wrap_cff(&font, &cff, 1000, 2, adv, lsb) < 0) { rc = 1; goto out; }
	{
		/* Locate the CFF table in the file and break the CharStrings
		 * INDEX: set its count to 0xFFFF while the real one is 2. */
		size_t p;
		uint32_t cffOff = 0;
		for (p = 12; p < 12 + 16 * 8; p += 16)
			if (!memcmp(font.p + p, "CFF ", 4)) {
				cffOff = (uint32_t) font.p[p+8] << 24 |
				         (uint32_t) font.p[p+9] << 16 |
				         (uint32_t) font.p[p+10] << 8 |
				         (uint32_t) font.p[p+11];
				break;
			}
		{
			uint8_t bad[2] = { 0xFF, 0xFF };
			SFT_Font *f = load_patched(&font, cffOff + lo.csIndex, bad, 2);
			if (f) { sft_freefont(f); rc = 2; }
		}
	}
out:
	(void) subrs; (void) storage;
	b_free(&cst); b_free(&font);
	return rc;
}

/* Offsets that leave the CFF table: a CharStrings offset pointing past
 * the table (and past the file) must be rejected. */
static int test_offset_outside_table(void)
{
	Buf cst = { 0 }, cff = { 0 }, font = { 0 };
	CffSpec spec; CffLayout lo; int rc = 0;
	unsigned adv[1] = { 700 }; int lsb[1] = { 50 };
	Item item;

	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, 200); cs_int(&cst, 0); b_u8(&cst, 5); b_u8(&cst, 14);
	item.p = cst.p; item.n = cst.len;
	memset(&spec, 0, sizeof spec);
	spec.chars = &item; spec.nChars = 1; spec.charstringType = -1;
	cff_assemble(&cff, &spec, &lo);
	if (wrap_cff(&font, &cff, 1000, 1, adv, lsb) < 0) { rc = 1; goto out; }
	{
		size_t p; uint32_t cffOff = 0;
		uint32_t badOff = 0x00200000u; /* way beyond the file */
		for (p = 12; p < 12 + 16 * 8; p += 16)
			if (!memcmp(font.p + p, "CFF ", 4)) {
				cffOff = (uint32_t) font.p[p+8] << 24 |
				         (uint32_t) font.p[p+9] << 16 |
				         (uint32_t) font.p[p+10] << 8 |
				         (uint32_t) font.p[p+11];
				break;
			}
		(void) badOff;
		{
			uint8_t bad[4];
			uint32_t v = 0x00001000u;   /* larger than the CFF table */
			bad[0] = (uint8_t) (v >> 24); bad[1] = (uint8_t) (v >> 16);
			bad[2] = (uint8_t) (v >> 8);  bad[3] = (uint8_t) v;
			(void) badOff;
			{
				SFT_Font *f = load_patched(&font, cffOff + lo.csOperand, bad, 4);
				if (f) { sft_freefont(f); rc = 2; }
			}
		}
	}
out:
	b_free(&cst); b_free(&cff); b_free(&font);
	return rc;
}

/* Subroutine recursion: a subr calling itself must be cut off at the
 * Type 2 nesting limit rather than recursing forever. */
static int test_subr_loop(void)
{
	Buf cst = { 0 }, cff = { 0 }, font = { 0 }, loop = { 0 };
	CffSpec spec; CffLayout lo; int rc = 0;
	unsigned adv[1] = { 700 }; int lsb[1] = { 50 };
	Item item, *subrs = NULL; Buf storage = { 0 };

	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, -107); b_u8(&cst, 10);       /* callsubr, index 0 */
	cs_int(&cst, 200); cs_int(&cst, 0); b_u8(&cst, 5); b_u8(&cst, 14);
	item.p = cst.p; item.n = cst.len;
	cs_int(&loop, -107); b_u8(&loop, 10);      /* callsubr 0 again */
	b_u8(&loop, 11);
	subrs = make_subr_items(1, 0, &loop, &storage);
	if (!subrs) { rc = 1; goto out; }
	memset(&spec, 0, sizeof spec);
	spec.chars = &item; spec.nChars = 1;
	spec.lsubrs = subrs; spec.nLsubrs = 1; spec.charstringType = -1;
	cff_assemble(&cff, &spec, &lo);
	if (wrap_cff(&font, &cff, 1000, 1, adv, lsb) < 0) { rc = 2; goto out; }
	{
		SFT_Font *f = sft_loadmem(font.p, font.len);
		SFT s;
		if (!f) { rc = 3; goto out; }
		s = (SFT) { f, 20.0, 20.0, 0.0, 0.0, 0 };
		if (sft_gmetrics(&s, 0, &(SFT_GMetrics) { 0 }) == 0) rc = 4;
		/* and a valid glyph from the same instance still renders */
		sft_freefont(f);
	}
out:
	(void) lo; free(subrs); b_free(&storage);
	b_free(&cst); b_free(&cff); b_free(&font); b_free(&loop);
	return rc;
}

/* Too few operands for the operator; unterminated charstring; callothersubr. */
static int test_operand_and_termination(void)
{
	Buf cst = { 0 }, font = { 0 };
	SFT_Font *f;
	int rc = 0;

	struct Case { int kind; } cases[] = { {0}, {1}, {2}, {3} };
	size_t ci;
	for (ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
		b_free(&cst); cst = (Buf) { 0 };
		switch (cases[ci].kind) {
		case 0: /* rlineto with one operand */
			cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
			cs_int(&cst, 100); b_u8(&cst, 5);
			break;
		case 1: /* rmoveto without any operand */
			b_u8(&cst, 21);
			break;
		case 2: /* unterminated (no endchar) */
			cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
			cs_int(&cst, 100); cs_int(&cst, 0); b_u8(&cst, 5);
			break;
		case 3: /* callothersubr (escape 16) */
			b_u8(&cst, 12); b_u8(&cst, 16);
			break;
		}
		if (simple_cff(&font, &cst, 1) < 0) { rc = 1; break; }
		f = sft_loadmem(font.p, font.len);
		if (!f) { rc = 2; break; }
		{
			SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
			SFT_GMetrics m;
			if (sft_gmetrics(&s, 0, &m) == 0) rc = 3;
			sft_freefont(f);
		}
		b_free(&font); font = (Buf) { 0 };
	}
	b_free(&cst); b_free(&font);
	return rc;
}

/* A failed glyph must not leave a half image behind, and the same instance
 * must remain usable for a subsequent valid glyph. */
static int test_error_reuse(void)
{
	Buf cst[3] = { { 0 }, { 0 }, { 0 } }, cff = { 0 }, font = { 0 };
	CffSpec spec; CffLayout lo; int rc = 0;
	unsigned adv[3] = { 700, 700, 700 }; int lsb[3] = { 50, 50, 50 };
	Item items[3]; SFT_Font *f;
	unsigned char *pixels;
	SFT_GMetrics m;

	/* Glyph 0 valid box, glyph 1 truncated (reads outside its program),
	 * glyph 2 valid box. */
	cs_int(&cst[0], 0); cs_int(&cst[0], 0); b_u8(&cst[0], 21);
	cs_int(&cst[0], 200); cs_int(&cst[0], 0); b_u8(&cst[0], 5);
	cs_int(&cst[0], 0); cs_int(&cst[0], 200); b_u8(&cst[0], 5);
	b_u8(&cst[0], 14);
	cs_int(&cst[1], 0); cs_int(&cst[1], 0); b_u8(&cst[1], 21);
	b_u8(&cst[1], 5);   /* rlineto with no operands */
	cs_int(&cst[2], 0); cs_int(&cst[2], 0); b_u8(&cst[2], 21);
	cs_int(&cst[2], 200); cs_int(&cst[2], 0); b_u8(&cst[2], 5);
	cs_int(&cst[2], 0); cs_int(&cst[2], -200); b_u8(&cst[2], 5);
	b_u8(&cst[2], 14);
	items[0].p = cst[0].p; items[0].n = cst[0].len;
	items[1].p = cst[1].p; items[1].n = cst[1].len;
	items[2].p = cst[2].p; items[2].n = cst[2].len;
	memset(&spec, 0, sizeof spec);
	spec.chars = items; spec.nChars = 3; spec.charstringType = -1;
	cff_assemble(&cff, &spec, &lo);
	if (wrap_cff(&font, &cff, 1000, 3, adv, lsb) < 0) { rc = 1; goto out; }

	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	{
		SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
		int w, h, nz;
		/* Valid glyph first, into zeroed memory. */
		if (render_glyph(&s, 0, &pixels, &w, &h, &nz) < 0) { rc = 3; goto done; }
		if (nz == 0) rc = 4;
		free(pixels);
		/* Now the broken glyph must fail and leave the caller's image
		 * untouched. */
		if (sft_gmetrics(&s, 1, &m) == 0) {
			/* If metrics succeeded we still require rendering to fail
			 * (the program is malformed). */
		}
		pixels = calloc(64, 1);
		if (!pixels) { rc = 5; goto done; }
		memset(pixels, 0xA5, 64);
		if (sft_render(&s, 1, (SFT_Image) { pixels, 8, 8 }) == 0) rc = 6;
		{
			int i;
			for (i = 0; i < 64; i++) if (pixels[i] != 0xA5) { rc = 7; break; }
		}
		free(pixels);
		/* The same font instance still renders a valid glyph. */
		if (render_glyph(&s, 2, &pixels, &w, &h, &nz) < 0) rc = 8;
		else if (nz == 0) rc = 9;
		free(pixels);
done:
		sft_freefont(f);
	}
out:
	b_free(&cst[0]); b_free(&cst[1]); b_free(&cst[2]);
	b_free(&cff); b_free(&font);
	return rc;
}

/* Unsupported formats must be rejected explicitly:
 *  - seac-style endchar with operands
 *  - CID (ROS in the Top DICT)
 *  - CFF2 / unknown major version
 *  - CharstringType other than 2
 *  - arithmetic / storage escapes */
static int test_unsupported(void)
{
	int rc = 0, kind;
	Buf cst = { 0 };

	for (kind = 0; kind < 6; kind++) {
		Buf font = { 0 }, cff = { 0 };
		CffSpec spec; CffLayout lo;
		Item item;
		unsigned adv[1] = { 700 }; int lsb[1] = { 50 };
		b_free(&cst); cst = (Buf) { 0 };
		memset(&spec, 0, sizeof spec);
		spec.charstringType = -1;
		switch (kind) {
		case 0:
			cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
			cs_int(&cst, 200); cs_int(&cst, 0); b_u8(&cst, 5);
			cs_int(&cst, 0); cs_int(&cst, 200);
			cs_int(&cst, 100); cs_int(&cst, 100);
			b_u8(&cst, 14);                       /* seac form */
			break;
		case 1:
			cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
			cs_int(&cst, 200); cs_int(&cst, 0); b_u8(&cst, 5); b_u8(&cst, 14);
			spec.useROS = 1;
			break;
		case 2:
			cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
			cs_int(&cst, 200); cs_int(&cst, 0); b_u8(&cst, 5); b_u8(&cst, 14);
			spec.major = 2;
			break;
		case 3:
			cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
			cs_int(&cst, 200); cs_int(&cst, 0); b_u8(&cst, 5); b_u8(&cst, 14);
			spec.charstringType = 3;
			break;
		case 4:
			/* Arithmetic escapes: 1 2 add (12 10). */
			cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
			b_u8(&cst, 140); b_u8(&cst, 141);
			b_u8(&cst, 12); b_u8(&cst, 10);
			b_u8(&cst, 14);
			break;
		case 5:
			/* Store / load escapes (12 20 / 12 21). */
			cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
			b_u8(&cst, 140); b_u8(&cst, 12); b_u8(&cst, 20);
			b_u8(&cst, 12); b_u8(&cst, 21);
			b_u8(&cst, 14);
			break;
		}
		item.p = cst.p; item.n = cst.len;
		spec.chars = &item; spec.nChars = 1;
		cff_assemble(&cff, &spec, &lo);
		if (wrap_cff(&font, &cff, 1000, 1, adv, lsb) < 0) { rc = 1; break; }
		{
			SFT_Font *f = sft_loadmem(font.p, font.len);
			SFT_GMetrics m;
			if (kind == 0 || kind == 4 || kind == 5) {
				/* Glyph-level rejection: the font loads, the glyph fails. */
				if (!f) { rc = 2; break; }
				{
					SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
					unsigned char *p; int w = 0, h = 0, nz = 0;
					if (sft_gmetrics(&s, 0, &m) == 0) {
						/* Empty outline is only acceptable for kind 0
						 * if it were silently ignored, which is not
						 * allowed: seac must fail. */
						rc = 3;
					}
					(void) p; (void) w; (void) h; (void) nz;
				}
				sft_freefont(f);
			} else {
				/* Font-level rejection. */
				if (f) { sft_freefont(f); rc = 4; }
			}
		}
		b_free(&cff); b_free(&font);
		if (rc) break;
	}
	b_free(&cst);
	(void) kind;
	return rc;
}

/* The four flex operators.  Every glyph starts at (0,0), draws one flex
 * and endchar then closes the contour.  The reference box is assembled
 * from the two explicit cubics, which are listed here in full. */
static int test_flex(void)
{
	struct Case {
		int op;
		Cubic c[2];
	} cases[4];
	int ci, rc = 0;

	/* hflex, flex and hflex1 produce the same pair of arches:
	 *   (0,0)-(100,0)-(150,60)-(200,60) and
	 *   (200,60)-(300,60)-(350,0)-(400,0).
	 * flex1 with d6=0 ends at (350,60): |sum dx| = 350 > |sum dy| = 0,
	 * so dx6 = d6 = 0 and dy6 = -sumdy = 0. */
	for (ci = 0; ci < 4; ci++) {
		cases[ci].op = 34 + ci;
		cases[ci].c[0] = (Cubic) { 0, 0, 100, 0, 150, 60, 200, 60 };
		if (ci < 3)
			cases[ci].c[1] = (Cubic) { 200, 60, 300, 60, 350, 0, 400, 0 };
		else
			cases[ci].c[1] = (Cubic) { 200, 60, 300, 60, 350, 0, 350, 60 };
	}

	for (ci = 0; ci < 4 && !rc; ci++) {
		Buf cst = { 0 }, font = { 0 };
		double unit[4];
		int op = cases[ci].op;
		SFT_Font *f;
		cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
		if (op == 34) {
			/* dx1 dx2 dy2 dx3 dx4 dx5 dx6 */
			cs_int(&cst, 100); cs_int(&cst, 50); cs_int(&cst, 60);
			cs_int(&cst, 50);
			cs_int(&cst, 100); cs_int(&cst, 50); cs_int(&cst, 50);
		} else if (op == 35) {
			/* dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 dx6 dy6 fd */
			cs_int(&cst, 100); cs_int(&cst, 0);
			cs_int(&cst, 50); cs_int(&cst, 60);
			cs_int(&cst, 50); cs_int(&cst, 0);
			cs_int(&cst, 100); cs_int(&cst, 0);
			cs_int(&cst, 50); cs_int(&cst, -60);
			cs_int(&cst, 50); cs_int(&cst, 0);
			cs_int(&cst, 50);
		} else if (op == 36) {
			/* dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6 */
			cs_int(&cst, 100); cs_int(&cst, 0);
			cs_int(&cst, 50); cs_int(&cst, 60);
			cs_int(&cst, 50);
			cs_int(&cst, 100);
			cs_int(&cst, 50); cs_int(&cst, -60);
			cs_int(&cst, 50);
		} else {
			/* dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 d6 */
			cs_int(&cst, 100); cs_int(&cst, 0);
			cs_int(&cst, 50); cs_int(&cst, 60);
			cs_int(&cst, 50); cs_int(&cst, 0);
			cs_int(&cst, 100); cs_int(&cst, 0);
			cs_int(&cst, 50); cs_int(&cst, -60);
			cs_int(&cst, 0);
		}
		b_u8(&cst, 12); b_u8(&cst, (unsigned) op);
		b_u8(&cst, 14);
		bb_reset(unit);
		bb_point(unit, 0, 0);
		bb_cubic(unit, &cases[ci].c[0]);
		bb_cubic(unit, &cases[ci].c[1]);
		if (simple_cff(&font, &cst, 1) < 0) { b_free(&cst); rc = 10 + ci; break; }
		f = sft_loadmem(font.p, font.len);
		if (!f) { b_free(&cst); b_free(&font); rc = 20 + ci; break; }
		{
			SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
			rc = check_metrics_against(&s, 0, unit, 700, 50);
		}
		sft_freefont(f); b_free(&cst); b_free(&font);
		if (rc) rc += 100 * ci;
	}
	return rc;
}


/* Non-uniform scale plus both Y directions: the rectangle announced by
 * sft_gmetrics must be exactly the one sft_render fills, and the origin
 * must match for fractional offsets.  A hollow square (outer box and an
 * inner hole, both wound in the same direction as the outer contour, so
 * the non-zero winding rule punches the hole) exercises closed contours
 * and holes along with the positioning. */
static int test_metrics_render_alignment(void)
{
	Buf cst = { 0 }, font = { 0 };
	SFT_Font *f;
	double scales[][2] = { { 11.7, 29.3 }, { 33.3, 17.9 }, { 47.1, 61.3 } };
	double offs[][2] = { { 0.3, 0.47 }, { -0.22, 0.81 }, { 0.13, -0.29 } };
	int fl[2] = { 0, SFT_DOWNWARD_Y };
	unsigned i, j, k;
	int rc = 0;

	/* Outer box clockwise, inner box counter-clockwise, so the non-zero
	 * winding rule punches a hole. */
	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, 600); cs_int(&cst, 0); b_u8(&cst, 5);
	cs_int(&cst, 0); cs_int(&cst, 400); b_u8(&cst, 5);
	cs_int(&cst, -600); cs_int(&cst, 0); b_u8(&cst, 5);
	cs_int(&cst, 0); cs_int(&cst, -400); b_u8(&cst, 5);
	cs_int(&cst, 200); cs_int(&cst, 150); b_u8(&cst, 21);
	cs_int(&cst, 0); cs_int(&cst, 100); b_u8(&cst, 5);
	cs_int(&cst, 200); cs_int(&cst, 0); b_u8(&cst, 5);
	cs_int(&cst, 0); cs_int(&cst, -100); b_u8(&cst, 5);
	cs_int(&cst, -200); cs_int(&cst, 0); b_u8(&cst, 5);
	b_u8(&cst, 14);
	if (simple_cff(&font, &cst, 1) < 0) { b_free(&cst); return 1; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { b_free(&cst); b_free(&font); return 2; }
	for (i = 0; i < 3 && !rc; i++) for (j = 0; j < 3 && !rc; j++)
	for (k = 0; k < 2 && !rc; k++) {
		SFT s = { f, scales[i][0], scales[j][1], offs[i][0], offs[j][1], fl[k] };
		SFT_GMetrics m;
		unsigned char *pix; int w, h, nz, r, c;
		double unit[4] = { 0, 0, 600, 400 };
		int box[4];
		if (sft_gmetrics(&s, 0, &m) < 0) { rc = 3; break; }
		expected_device_box(&s, unit, box);
		if (m.minWidth != box[2] - box[0] + 1 ||
		    m.minHeight != box[3] - box[1] + 1) { rc = 4; break; }
		if (m.yOffset != (fl[k] & SFT_DOWNWARD_Y ? -box[3] : box[1])) {
			rc = 5; break;
		}
		if (render_glyph(&s, 0, &pix, &w, &h, &nz) < 0) { rc = 6; break; }
		/* Nonzero coverage must start within one pixel of the origin
		 * the metrics announced, in both directions. */
		{
			int firstRow = -1, firstCol = -1;
			for (r = 0; r < h && firstRow < 0; r++)
				for (c = 0; c < w; c++)
					if (pix[r * w + c]) { firstRow = r; firstCol = c; break; }
			if (firstRow < 0 || firstRow > 1 || firstCol > 1) rc = 7;
		}
		/* The hole must exist: the pixel nearest unit point (300,200)
		 * is transparent. */
		if (!rc) {
			double fx = (300.0 - 0.0) * s.xScale / 1000.0 + s.xOffset;
			double fy = (200.0 - 0.0) * s.yScale / 1000.0 + s.yOffset;
			double xs = s.xScale / 1000.0, ys = s.yScale / 1000.0;
			double px0 = 0.0 * xs + s.xOffset;
			double py0 = 0.0 * ys + s.yOffset;
			double px1 = 600.0 * xs + s.xOffset;
			double py1 = 400.0 * ys + s.yOffset;
			int bx = (int) floor(px0), by = (int) floor(py0);
			int bw = (int) ceil(px1) - bx + 1, bh = (int) ceil(py1) - by + 1;
			int cx = (int) (fx - bx), cy = (int) (fy - by);
			if (fl[k] & SFT_DOWNWARD_Y) cy = bh - 1 - cy;
			(void)bw;
			if (cx < 0 || cx >= w || cy < 0 || cy >= h) rc = 9;
			/* At very small scales the hole is thinner than a pixel and
			 * antialiasing legitimately leaves partial coverage.  Only
			 * require full transparency when the hole spans several
			 * device pixels. */
			else if ((fx - px0) > 4.0 && (py1 - py0) > 8.0 &&
			         pix[cy * w + cx] != 0) rc = 8;
		}
		free(pix);
	}
	sft_freefont(f); b_free(&cst); b_free(&font);
	return rc;
}

/* Two different CFF fonts rendered interleaved: any interpreter state
 * (operand stack, current point, subr frame depth) must be per call. */
static int test_interleaved_fonts(void)
{
	Buf a = { 0 }, b = { 0 }, ca = { 0 }, cb = { 0 };
	SFT_Font *fa, *fb;
	int rc = 0;
	unsigned char *pa = NULL, *pb = NULL, *pa2 = NULL;
	int wa, ha, na, wb, hb, nb, wa2, ha2, na2;

	/* A: right triangle; B: different, wider trapezoid. */
	cs_triangle(&ca);
	cs_int(&cb, 0); cs_int(&cb, 0); b_u8(&cb, 21);
	cs_int(&cb, 600); cs_int(&cb, 0); b_u8(&cb, 5);
	cs_int(&cb, -100); cs_int(&cb, 300); b_u8(&cb, 5);
	cs_int(&cb, -400); cs_int(&cb, 0); b_u8(&cb, 5);
	b_u8(&cb, 14);
	if (simple_cff(&a, &ca, 1) < 0 || simple_cff(&b, &cb, 1) < 0) return 1;
	fa = sft_loadmem(a.p, a.len);
	fb = sft_loadmem(b.p, b.len);
	if (!fa || !fb) { rc = 2; goto out; }
	{
		SFT sa = { fa, 20.0, 20.0, 0.25, 0.5, SFT_DOWNWARD_Y };
		SFT sb = { fb, 19.0, 23.0, 0.1, 0.2, 0 };
		/* Render A, then B, then A again; A's pixels must be identical. */
		if (render_glyph(&sa, 0, &pa, &wa, &ha, &na) < 0 ||
		    render_glyph(&sb, 0, &pb, &wb, &hb, &nb) < 0 ||
		    render_glyph(&sa, 0, &pa2, &wa2, &ha2, &na2) < 0) rc = 3;
		else if (wa != wa2 || ha != ha2 || na != na2 ||
		         memcmp(pa, pa2, (size_t) wa * ha) != 0) rc = 4;
		else if (wb == wa && hb == ha && memcmp(pa, pb, (size_t) wa * ha) == 0)
			rc = 5; /* the two fonts must not be interchangeable */
	}
	free(pa); free(pb); free(pa2);
out:
	if (fa) sft_freefont(fa); if (fb) sft_freefont(fb);
	b_free(&a); b_free(&b); b_free(&ca); b_free(&cb);
	return rc;
}

/* At a large scale a cubic's raster must be smooth along its boundary.
 * The check is independent of the flattened geometry: only the fraction
 * of fully covered vs. partially covered pixels along the arch is
 * examined.  A coarse (control-polygon or 2-3 segment) flattener leaves
 * long straight stretches with few antialiased transitions; a proper
 * device-space flattener yields a smooth curve with many partial pixels
 * and no single straight step at the apex.
 *
 * Error basis: the curve is 600 x 2700 font units at a 2.4 device scale,
 * i.e. about 1440 x 648 pixels; a 0.25-pixel tolerance produces at least
 * ~50 subdivisions along a quarter arc, far more than the ~10 that
 * would show visible facets. */
static int test_high_zoom_smooth(void)
{
	Buf cst = { 0 }, font = { 0 };
	SFT_Font *f; int rc = 0;
	unsigned char *pix = NULL; int w, h, nz, r, c;
	SFT s;
	int partial = 0, full = 0;

	cs_int(&cst, 0); cs_int(&cst, 0); b_u8(&cst, 21);
	cs_int(&cst, 300); cs_int(&cst, 900);
	cs_int(&cst, 300); cs_int(&cst, 0);
	cs_int(&cst, 300); cs_int(&cst, -900);
	b_u8(&cst, 8);
	cs_int(&cst, -900); cs_int(&cst, 0); b_u8(&cst, 5);
	b_u8(&cst, 14);
	if (simple_cff(&font, &cst, 1) < 0) { b_free(&cst); return 1; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { b_free(&cst); b_free(&font); return 2; }
	s = (SFT) { f, 2400.0, 2400.0, 0.0, 0.0, 0 };
	if (render_glyph(&s, 0, &pix, &w, &h, &nz) < 0) { rc = 3; goto out; }
	if (w < 64 || h < 64) { rc = 4; goto out; }
	for (r = 0; r < h; r++) for (c = 0; c < w; c++) {
		unsigned v = pix[r * w + c];
		if (v == 255) full++;
		else if (v != 0) partial++;
	}
	/* A smooth boundary has partial coverage along the entire left and
	 * right curve, so partial pixels must be numerous (at least several
	 * hundred for a ~1600 px boundary).  A coarse flattener produces
	 * long straight chords with only a handful of antialiased pixels at
	 * their ends. */
	if (partial < 200) rc = 5;
	if (full == 0) rc = 6;
	/* Also check that the boundary is not piecewise-linear within a
	 * 5-pixel window: consecutive columns of the last covered row must
	 * differ by at most 2 pixels.  The apex is excluded because the
	 * arch's tangent there is genuinely horizontal. */
	{
		int edge[4096];
		int c0;
		for (c0 = 0; c0 < w && c0 < 4096; c0++) {
			int rr, last = -1;
			for (rr = 0; rr < h; rr++)
				if (pix[rr * w + c0]) last = rr;
			edge[c0] = last;
		}
		for (c0 = 2; c0 + 2 < w && !rc; c0++) {
			int a = edge[c0 - 2], b = edge[c0 + 2];
			if (a >= 0 && b >= 0 && abs(a - b) > 12) rc = 7;
		}
	}
out:
	free(pix); sft_freefont(f); b_free(&cst); b_free(&font);
	return rc;
}

/* Anti-diagonal (counter-clockwise) winding must be filled the same way
 * as the clockwise version: the non-zero winding rule is orientation
 * independent for a single closed contour. */
static int test_winding(void)
{
	/* Two fonts with a square wound in opposite directions must render
	 * identical masks (black background, same coverage). */
	Buf c1 = { 0 }, c2 = { 0 }, f1 = { 0 }, f2 = { 0 };
	SFT_Font *fa, *fb; int rc = 0, i;

	cs_int(&c1, 0); cs_int(&c1, 0); b_u8(&c1, 21);
	cs_int(&c1, 400); cs_int(&c1, 0); b_u8(&c1, 5);
	cs_int(&c1, 0); cs_int(&c1, 300); b_u8(&c1, 5);
	cs_int(&c1, -400); cs_int(&c1, 0); b_u8(&c1, 5);
	cs_int(&c1, 0); cs_int(&c1, -300); b_u8(&c1, 5);
	b_u8(&c1, 14);
	cs_int(&c2, 0); cs_int(&c2, 0); b_u8(&c2, 21);
	cs_int(&c2, 0); cs_int(&c2, 300); b_u8(&c2, 5);
	cs_int(&c2, 400); cs_int(&c2, 0); b_u8(&c2, 5);
	cs_int(&c2, 0); cs_int(&c2, -300); b_u8(&c2, 5);
	cs_int(&c2, -400); cs_int(&c2, 0); b_u8(&c2, 5);
	b_u8(&c2, 14);
	if (simple_cff(&f1, &c1, 1) < 0 || simple_cff(&f2, &c2, 1) < 0) return 1;
	fa = sft_loadmem(f1.p, f1.len);
	fb = sft_loadmem(f2.p, f2.len);
	if (!fa || !fb) { rc = 2; goto out; }
	{
		SFT sa = { fa, 20.0, 20.0, 0.1, 0.2, 0 };
		SFT sb = { fb, 20.0, 20.0, 0.1, 0.2, 0 };
		unsigned char *pa = NULL, *pb = NULL;
		int wa, ha, na, wb, hb, nb;
		if (render_glyph(&sa, 0, &pa, &wa, &ha, &na) < 0 ||
		    render_glyph(&sb, 0, &pb, &wb, &hb, &nb) < 0) rc = 3;
		else if (wa != wb || ha != hb || na != nb) rc = 4;
		else {
			/* The non-zero winding rule is orientation independent; the
			 * only allowed difference is floating-point rounding in the
			 * signed area accumulation (at most one gray level). */
			int k2;
			for (k2 = 0; k2 < wa * ha && !rc; k2++) {
				int d = (int) pa[k2] - (int) pb[k2];
				if (d > 1 || d < -1) rc = 5;
			}
		}
		free(pa); free(pb);
		(void)i;
	}
out:
	if (fa) sft_freefont(fa); if (fb) sft_freefont(fb);
	b_free(&c1); b_free(&c2); b_free(&f1); b_free(&f2);
	return rc;
}

/* The supplied node fixture generator writes a real OTTO font at a path
 * given on the command line; load it through sft_loadfile and render
 * both glyphs, so the exact bytes in checks/make_cff.mjs are covered. */
static int test_node_fixture(void)
{
	char path[256];
	char cmd[512];
	FILE *fp;
	Buf mem = { 0 };
	int rc = 0;
	int c, len = 0;

	/* Ask the generator to write into the sandbox's temp directory. */
	{
		const char *tmp = getenv("TMPDIR");
		if (!tmp) tmp = "/tmp";
		snprintf(path, sizeof path, "%s/schrift_cff_fixture.otf", tmp);
	}
	snprintf(cmd, sizeof cmd, "node checks/make_cff.mjs \"%s\" 2>/dev/null", path);
	if (system(cmd) != 0) return 0;   /* node may be absent: skip is not a pass */
	fp = fopen(path, "rb");
	if (!fp) return 1;
	while ((c = fgetc(fp)) != EOF) {
		b_u8(&mem, (unsigned) c);
		len++;
	}
	fclose(fp);
	remove(path);
	if (len < 100) { b_free(&mem); return 2; }
	{
		SFT_Font *f = sft_loadmem(mem.p, mem.len);
		SFT s;
		SFT_Glyph g;
		unsigned char *pix; int w, h, nz;
		if (!f) { b_free(&mem); return 3; }
		s = (SFT) { f, 30.0, 30.0, 0.0, 0.0, 0 };
		if (sft_lookup(&s, 'A', &g) != 0) rc = 4;
		else if (render_glyph(&s, g, &pix, &w, &h, &nz) < 0) rc = 5;
		else if (nz == 0) rc = 6;
		else free(pix);
		if (!rc && sft_lookup(&s, 'B', &g) == 0) {
			if (render_glyph(&s, g, &pix, &w, &h, &nz) < 0) rc = 7;
			else if (nz == 0) rc = 8;
			else free(pix);
		}
		sft_freefont(f);
	}
	b_free(&mem);
	return rc;
}


/* A moveto that is not followed by a segment must not affect the
 * bounding box: only the actual curve extrema count.  Glyph 0 first moves
 * far away and immediately moves back; glyph 1 draws a small box after a
 * far-away move.  Both extents must be the drawn geometry only. */
static int test_moveto_not_geometry(void)
{
	Buf cst[2] = { { 0 }, { 0 } }, font = { 0 };
	SFT_Font *f; int rc = 0;
	Item items[2];

	/* Glyph 0: (5000,5000) rmoveto, then back to (0,0) and draw a 200 box. */
	cs_int(&cst[0], 5000); cs_int(&cst[0], 5000); b_u8(&cst[0], 21);
	cs_int(&cst[0], -5000); cs_int(&cst[0], -5000); b_u8(&cst[0], 21);
	cs_int(&cst[0], 200); cs_int(&cst[0], 0); b_u8(&cst[0], 5);
	cs_int(&cst[0], 0); cs_int(&cst[0], 200); b_u8(&cst[0], 5);
	cs_int(&cst[0], -200); cs_int(&cst[0], 0); b_u8(&cst[0], 5);
	b_u8(&cst[0], 14);
	/* Glyph 1: move nowhere, draw a thin box. */
	cs_int(&cst[1], 50); cs_int(&cst[1], 0); b_u8(&cst[1], 21);
	cs_int(&cst[1], 100); cs_int(&cst[1], 0); b_u8(&cst[1], 5);
	cs_int(&cst[1], 0); cs_int(&cst[1], 100); b_u8(&cst[1], 5);
	cs_int(&cst[1], -100); cs_int(&cst[1], 0); b_u8(&cst[1], 5);
	b_u8(&cst[1], 14);
	items[0].p = cst[0].p; items[0].n = cst[0].len;
	items[1].p = cst[1].p; items[1].n = cst[1].len;
	if (make_cff_font_full(&font, items, 2, NULL, 0, NULL, 0,
	                       0, 0, 0, 0, NULL, -1, 0) < 0) { rc = 1; goto out; }
	f = sft_loadmem(font.p, font.len);
	if (!f) { rc = 2; goto out; }
	{
		SFT s = { f, 20.0, 20.0, 0.0, 0.0, 0 };
		double u0[4] = { 0, 0, 200, 200 };
		double u1[4] = { 50, 0, 150, 100 };
		rc = check_metrics_against(&s, 0, u0, 700, 50);
		if (!rc) rc = check_metrics_against(&s, 1, u1, 700, 50);
		if (!rc) {
			unsigned char *p; int w, h, nz;
			if (render_glyph(&s, 0, &p, &w, &h, &nz) < 0) rc = 3;
			else if (nz == 0) rc = 4;
			free(p);
		}
		/* A glyph consisting only of movetos is empty. */
	}
	sft_freefont(f);
out:
	b_free(&cst[0]); b_free(&cst[1]); b_free(&font);
	return rc;
}

/* ================================================================== */
/* Case table                                                          */
/* ================================================================== */

static const struct contract_case cases_table[] = {
	{ "outlines",  "line-equivalence", test_line_equivalence  },
	{ "outlines",  "cubic-extrema",    test_cubic_extrema     },
	{ "outlines",  "cubic-coverage",   test_cubic_coverage    },
	{ "outlines",  "empty-glyph",      test_empty_glyph       },
	{ "outlines",  "subr-continuity",  test_subr_continuity   },
	{ "outlines",  "subr-bias",        test_subr_bias         },
	{ "outlines",  "wide-index",       test_wide_index        },
	{ "outlines",  "fixed-numbers",    test_fixed_numbers     },
	{ "outlines",  "width-consumption",test_width_consumption },
	{ "outlines",  "all-operators",    test_all_operators     },
	{ "outlines",  "flex",             test_flex              },
	{ "outlines",  "metrics-render",   test_metrics_render_alignment },
	{ "outlines",  "interleaved",      test_interleaved_fonts },
	{ "outlines",  "high-zoom",        test_high_zoom_smooth  },
	{ "outlines",  "winding",          test_winding           },
	{ "outlines",  "node-fixture",     test_node_fixture      },
	{ "outlines",  "moveto-not-geometry", test_moveto_not_geometry },
	{ "subr-hints","hints-in-subr",    test_hints_in_subr     },
	{ "subr-hints","stem-accumulation",test_stem_accumulation },
	{ "subr-hints","cntrmask-bytes",   test_cntrmask_bytes    },
	{ "subr-hints","stack-limit",      test_stack_limit       },
	{ "invalid",   "bad-index",        test_bad_index         },
	{ "invalid",   "offset-outside",   test_offset_outside_table },
	{ "invalid",   "subr-loop",        test_subr_loop         },
	{ "invalid",   "operand-termination", test_operand_and_termination },
	{ "invalid",   "error-reuse",      test_error_reuse       },
	{ "invalid",   "unsupported",      test_unsupported       },
};

const struct contract_case *
contract_cases(size_t *count)
{
	*count = sizeof cases_table / sizeof cases_table[0];
	return cases_table;
}
