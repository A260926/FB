/* This file is part of libschrift.
 *
 * © 2019-2022 Thomas Oltmann and contributors
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

/* A small, self-contained CFF1 (name-keyed, CharstringType 2) decoder.
 *
 * It never draws anything itself: geometry is handed to a sink provided by
 * the caller, which lets the TrueType based rasterizer in schrift.c consume
 * cubic outlines without knowing anything about CFF.  The tight bounding
 * box is computed here from the actual cubic segments (including their
 * interior extrema), never from control polygons or the font's FontBBox.
 *
 * Everything below is immutable with respect to the font: no state outlives
 * a single sfti_cff_decode() call, so interleaved rendering of different
 * fonts cannot interfere with each other. */

#include "schrift_cff.h"

#include <math.h>
#include <string.h>

#define CFF_MAX_STACK       48
#define CFF_MAX_SUBR_DEPTH  10
#define CFF_MAX_OPERANDS    48

/* ------------------------------------------------------------------ */
/* Bounds checked readers.  Every offset is relative to the beginning */
/* of the font memory, and `limit` is the exclusive end of the table   */
/* (or of the program currently being interpreted).                    */

static int
cff_range(uint32_t off, uint32_t need, uint32_t limit)
{
	return off <= limit && need <= limit - off;
}

static int
cff_u8(const uint8_t *mem, uint32_t limit, uint32_t off, uint8_t *out)
{
	if (!cff_range(off, 1, limit))
		return -1;
	*out = mem[off];
	return 0;
}

static int
cff_u16(const uint8_t *mem, uint32_t limit, uint32_t off, uint16_t *out)
{
	if (!cff_range(off, 2, limit))
		return -1;
	*out = (uint16_t) ((uint16_t) mem[off] << 8 | mem[off + 1]);
	return 0;
}

static int
cff_i16(const uint8_t *mem, uint32_t limit, uint32_t off, int16_t *out)
{
	uint16_t v;
	if (cff_u16(mem, limit, off, &v) < 0)
		return -1;
	*out = (int16_t) v;
	return 0;
}

static int
cff_u32(const uint8_t *mem, uint32_t limit, uint32_t off, uint32_t *out)
{
	if (!cff_range(off, 4, limit))
		return -1;
	*out = (uint32_t) mem[off] << 24 | (uint32_t) mem[off + 1] << 16 |
	       (uint32_t) mem[off + 2] << 8 | mem[off + 3];
	return 0;
}

static int
cff_i32(const uint8_t *mem, uint32_t limit, uint32_t off, int32_t *out)
{
	uint32_t v;
	if (cff_u32(mem, limit, off, &v) < 0)
		return -1;
	*out = (int32_t) v;
	return 0;
}

/* ------------------------------------------------------------------ */
/* INDEX handling.  Variable width (1..4 byte) offsets, 1-based.      */

typedef struct CFFIndex
{
	uint32_t offset;
	uint32_t count;
	uint32_t offSize;
	uint32_t data;
	uint32_t dataEnd;
} CFFIndex;

static uint32_t
cff_index_offset_value(const uint8_t *mem, uint32_t pos, uint32_t offSize)
{
	uint32_t v = 0, i;
	for (i = 0; i < offSize; i++)
		v = v << 8 | mem[pos + i];
	return v;
}

static int
cff_index_read(const uint8_t *mem, uint32_t limit, uint32_t off, CFFIndex *ix)
{
	uint16_t count;
	uint8_t  offSize;
	uint32_t arr, base, i, prev, cur, dataLen;

	memset(ix, 0, sizeof *ix);
	if (cff_u16(mem, limit, off, &count) < 0)
		return -1;
	ix->offset = off;
	ix->count  = count;
	if (count == 0) {
		ix->data = ix->dataEnd = off + 2;
		return 0;
	}
	if (cff_u8(mem, limit, off + 2, &offSize) < 0)
		return -1;
	if (offSize < 1 || offSize > 4)
		return -1;
	ix->offSize = offSize;
	arr = off + 3;
	if (!cff_range(arr, (uint32_t) count * offSize + offSize, limit))
		return -1;
	prev = 0;
	for (i = 0; i <= (uint32_t) count; i++) {
		cur = cff_index_offset_value(mem, arr + i * offSize, offSize);
		if (i == 0) {
			/* The CFF specification mandates that the first offset is 1. */
			if (cur != 1)
				return -1;
		} else if (cur < prev) {
			return -1;
		}
		prev = cur;
	}
	base    = arr + ((uint32_t) count + 1) * offSize;
	dataLen = prev - 1;
	if (!cff_range(base, dataLen, limit))
		return -1;
	ix->data    = base;
	ix->dataEnd = base + dataLen;
	return 0;
}

static int
cff_index_item(const uint8_t *mem, const CFFIndex *ix, uint32_t i,
               uint32_t *start, uint32_t *end)
{
	uint32_t o0, o1;
	if (ix->count == 0 || i >= ix->count)
		return -1;
	o0 = cff_index_offset_value(mem, ix->offset + 3 + i * ix->offSize,
	                             ix->offSize);
	o1 = cff_index_offset_value(mem, ix->offset + 3 + (i + 1) * ix->offSize,
	                             ix->offSize);
	if (o1 < o0 || o0 < 1)
		return -1;
	*start = ix->data + o0 - 1;
	*end   = ix->data + o1 - 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Number parsing.                                                     */

/* DICT real number, encoded as packed nibbles.  Implemented by hand so */
/* that the result does not depend on the process' locale.              */
static int
cff_real_number(const uint8_t *mem, uint32_t limit, uint32_t *pos, double *out)
{
	int neg = 0, seenDot = 0, seenExp = 0, expNeg = 0, any = 0;
	int exp = 0, k;
	double ip = 0.0, frac = 0.0, fdiv = 1.0, v, p;
	uint8_t b;
	uint8_t nib;
	int j;

	for (;;) {
		if (cff_u8(mem, limit, *pos, &b) < 0)
			return -1;
		(*pos)++;
		for (k = 0; k < 2; k++) {
			nib = k ? (uint8_t) (b & 0x0F) : (uint8_t) (b >> 4);
			if (nib == 0x0F) {
				if (!any)
					return -1;
				v = ip + frac / fdiv;
				if (seenExp) {
					if (exp > 308)
						return -1;
					p = 1.0;
					for (j = 0; j < exp; j++)
						p *= 10.0;
					if (expNeg)
						v /= p;
					else
						v *= p;
				}
				*out = neg ? -v : v;
				return 0;
			} else if (nib <= 9) {
				any = 1;
				if (seenExp) {
					if (exp < 1000)
						exp = exp * 10 + nib;
				} else if (seenDot) {
					frac = frac * 10.0 + nib;
					fdiv *= 10.0;
				} else {
					ip = ip * 10.0 + nib;
				}
			} else if (nib == 0x0A) {
				if (seenDot || seenExp)
					return -1;
				seenDot = 1;
			} else if (nib == 0x0B) {
				if (seenExp)
					return -1;
				seenExp = 1;
			} else if (nib == 0x0C) {
				if (seenExp)
					return -1;
				seenExp = 1;
				expNeg  = 1;
			} else if (nib == 0x0D) {
				return -1; /* reserved */
			} else { /* 0x0E: minus sign, only valid in front */
				if (k != 0 || any || seenDot || seenExp || neg)
					return -1;
				neg = 1;
			}
		}
	}
}

/* A number in a DICT.  Byte 255 is reserved there, unlike in charstrings. */
static int
cff_dict_number(const uint8_t *mem, uint32_t limit, uint32_t *pos, double *out)
{
	uint8_t b, c;
	int16_t i16;
	int32_t i32;

	if (cff_u8(mem, limit, *pos, &b) < 0)
		return -1;
	if (b == 28) {
		if (cff_i16(mem, limit, *pos + 1, &i16) < 0)
			return -1;
		*pos += 3;
		*out = i16;
		return 0;
	}
	if (b == 29) {
		if (cff_i32(mem, limit, *pos + 1, &i32) < 0)
			return -1;
		*pos += 5;
		*out = i32;
		return 0;
	}
	if (b == 30) {
		(*pos)++;
		return cff_real_number(mem, limit, pos, out);
	}
	if (b >= 32 && b <= 246) {
		*pos += 1;
		*out = (double) b - 139.0;
		return 0;
	}
	if (b >= 247 && b <= 250) {
		if (cff_u8(mem, limit, *pos + 1, &c) < 0)
			return -1;
		*pos += 2;
		*out = (double) ((b - 247) * 256 + c + 108);
		return 0;
	}
	if (b >= 251 && b <= 254) {
		if (cff_u8(mem, limit, *pos + 1, &c) < 0)
			return -1;
		*pos += 2;
		*out = -(double) ((b - 251) * 256 + c + 108);
		return 0;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* DICT scanning.  Both the Top DICT and the Private DICT are plain    */
/* operator/operand sequences; unknown-but-well-formed operators are   */
/* skipped, malformed ones are rejected.                               */

static int
cff_dict_is_number(uint8_t b)
{
	return (b >= 32 && b <= 254) || b == 28 || b == 29 || b == 30;
}

static int
cff_scan_top(const uint8_t *mem, uint32_t limit, uint32_t start, uint32_t end,
             double *charStrings, int *haveCharStrings,
             double *privateSize, double *privateOffset, int *havePrivate,
             int *charstringType)
{
	double ops[CFF_MAX_OPERANDS];
	int n = 0;
	uint32_t pos = start;
	uint8_t b, e;

	while (pos < end) {
		b = mem[pos];
		if (b == 12) {
			if (!cff_range(pos + 1, 1, end))
				return -1;
			e = mem[pos + 1];
			pos += 2;
			/* CID-keyed fonts, FDArray/FDSelect and ROS are not supported. */
			if (e == 30 || e == 36 || e == 37)
				return -1;
			if (e == 6) {
				if (n < 1)
					return -1;
				if (ops[n - 1] != 2.0)
					return -1;
				*charstringType = 2;
			}
			if (e == 7) {
				/* Only the default 1/1000 em square is supported. */
				if (n < 6 || ops[0] != 0.001 || ops[1] != 0.0 ||
				    ops[2] != 0.0 || ops[3] != 0.001 ||
				    ops[4] != 0.0 || ops[5] != 0.0)
					return -1;
			}
			n = 0;
			continue;
		}
		if (cff_dict_is_number(b)) {
			double v;
			if (cff_dict_number(mem, limit, &pos, &v) < 0)
				return -1;
			if (n >= CFF_MAX_OPERANDS)
				return -1;
			ops[n++] = v;
			continue;
		}
		if (b > 21)
			return -1;
		pos++;
		switch (b) {
		case 17: /* CharStrings */
			if (n < 1)
				return -1;
			*charStrings     = ops[n - 1];
			*haveCharStrings = 1;
			break;
		case 18: /* Private: size then offset */
			if (n < 2)
				return -1;
			*privateSize   = ops[n - 2];
			*privateOffset = ops[n - 1];
			*havePrivate   = 1;
			break;
		default:
			break;
		}
		n = 0;
	}
	return 0;
}

static int
cff_scan_private(const uint8_t *mem, uint32_t limit, uint32_t start,
                 uint32_t end, double *subrs, int *haveSubrs,
                 int32_t *defaultWidthX, int32_t *nominalWidthX)
{
	double ops[CFF_MAX_OPERANDS];
	int n = 0;
	uint32_t pos = start;
	uint8_t b;

	while (pos < end) {
		b = mem[pos];
		if (b == 12) {
			if (!cff_range(pos + 1, 1, end))
				return -1;
			pos += 2;
			n = 0;
			continue;
		}
		if (cff_dict_is_number(b)) {
			double v;
			if (cff_dict_number(mem, limit, &pos, &v) < 0)
				return -1;
			if (n >= CFF_MAX_OPERANDS)
				return -1;
			ops[n++] = v;
			continue;
		}
		if (b > 21)
			return -1;
		pos++;
		switch (b) {
		case 19: /* Subrs, relative to the Private DICT start */
			if (n < 1)
				return -1;
			*subrs     = ops[n - 1];
			*haveSubrs = 1;
			break;
		case 20:
			if (n < 1 || ops[n - 1] < -2147483647.0 ||
			    ops[n - 1] > 2147483647.0)
				return -1;
			*defaultWidthX = (int32_t) ops[n - 1];
			break;
		case 21:
			if (n < 1 || ops[n - 1] < -2147483647.0 ||
			    ops[n - 1] > 2147483647.0)
				return -1;
			*nominalWidthX = (int32_t) ops[n - 1];
			break;
		default:
			break;
		}
		n = 0;
	}
	return 0;
}

/* Converts an integral DICT operand into an absolute offset. */
static int
cff_abs_operand(double v, uint32_t base, uint32_t limit, uint32_t *out)
{
	uint32_t rel;
	if (v < 0.0 || v > 4294967295.0)
		return -1;
	rel = (uint32_t) v;
	if ((double) rel != v)
		return -1;
	if (base > 4294967295U - rel)
		return -1;
	if (base + rel > limit)
		return -1;
	*out = base + rel;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry points.                                                */

int
sfti_cff_init(const uint8_t *memory, uint32_t memorySize,
              uint32_t tableOffset, uint32_t tableSize, uint32_t numGlyphs,
              SFT_CFF *cff)
{
	CFFIndex name, top, strings, globalSubrs, charStrings;
	uint32_t limit, pos, topStart, topEnd, abs;
	uint8_t major, hdrSize;
	double csRel = 0.0, privSize = 0.0, privOffRel = 0.0;
	int haveCS = 0, havePriv = 0, charstringType = 2;
	int result = -1;

	memset(cff, 0, sizeof *cff);
	if (tableOffset > memorySize || tableSize > memorySize - tableOffset)
		return -1;
	limit = tableOffset + tableSize;

	if (!cff_range(tableOffset, 4, limit))
		return -1;
	major = memory[tableOffset];
	if (major != 1) /* CFF2 and other versions are unsupported. */
		return -1;
	hdrSize = memory[tableOffset + 2];
	if (hdrSize < 4 || !cff_range(tableOffset, hdrSize, limit))
		return -1;

	pos = tableOffset + hdrSize;
	if (cff_index_read(memory, limit, pos, &name) < 0)
		return -1;
	/* Font sets (more than one font) are out of scope. */
	if (name.count != 1)
		return -1;
	pos = name.dataEnd;

	if (cff_index_read(memory, limit, pos, &top) < 0)
		return -1;
	if (top.count != 1)
		return -1;
	if (cff_index_item(memory, &top, 0, &topStart, &topEnd) < 0)
		return -1;
	pos = top.dataEnd;

	if (cff_index_read(memory, limit, pos, &strings) < 0)
		return -1;
	pos = strings.dataEnd;

	if (cff_index_read(memory, limit, pos, &globalSubrs) < 0)
		return -1;
	cff->globalSubrs      = pos;
	cff->globalSubrsCount = globalSubrs.count;

	if (cff_scan_top(memory, limit, topStart, topEnd,
	                 &csRel, &haveCS, &privSize, &privOffRel, &havePriv,
	                 &charstringType) < 0)
		return -1;
	if (!haveCS || charstringType != 2)
		return -1;

	if (cff_abs_operand(csRel, tableOffset, limit, &abs) < 0)
		return -1;
	if (cff_index_read(memory, limit, abs, &charStrings) < 0)
		return -1;
	if (numGlyphs != 0 && charStrings.count != numGlyphs)
		return -1;
	cff->charStrings      = abs;
	cff->charStringsCount = charStrings.count;
	cff->charstringType   = charstringType;

	if (havePriv) {
		uint32_t privAbs, privLen;
		double subrsRel = 0.0;
		int haveSubrs = 0;

		if (privSize < 0.0 || privSize > 4294967295.0)
			return -1;
		privLen = (uint32_t) privSize;
		if ((double) privLen != privSize)
			return -1;
		if (cff_abs_operand(privOffRel, tableOffset, limit, &privAbs) < 0)
			return -1;
		if (!cff_range(privAbs, privLen, limit))
			return -1;
		cff->privateOffset = privAbs;
		cff->privateSize   = privLen;
		if (cff_scan_private(memory, limit, privAbs, privAbs + privLen,
		                     &subrsRel, &haveSubrs,
		                     &cff->defaultWidthX, &cff->nominalWidthX) < 0)
			return -1;
		if (haveSubrs) {
			CFFIndex local;
			if (cff_abs_operand(subrsRel, privAbs, limit, &abs) < 0)
				return -1;
			if (cff_index_read(memory, limit, abs, &local) < 0)
				return -1;
			cff->localSubrs      = abs;
			cff->localSubrsCount = local.count;
		}
	}

	cff->base = tableOffset;
	cff->size = tableSize;
	result = 0;
	return result;
}

/* ------------------------------------------------------------------ */
/* Bounding box of real cubic segments.                                */

typedef struct CFFBBox
{
	double minX, minY, maxX, maxY;
	int    any;
} CFFBBox;

static void
cff_bbox_add(CFFBBox *box, double x, double y)
{
	if (!box->any) {
		box->minX = box->maxX = x;
		box->minY = box->maxY = y;
		box->any = 1;
		return;
	}
	if (x < box->minX) box->minX = x;
	if (x > box->maxX) box->maxX = x;
	if (y < box->minY) box->minY = y;
	if (y > box->maxY) box->maxY = y;
}

static double
cff_cubic_at(double p0, double p1, double p2, double p3, double t)
{
	double mt = 1.0 - t;
	return mt * mt * mt * p0 + 3.0 * mt * mt * t * p1 +
	       3.0 * mt * t * t * p2 + t * t * t * p3;
}

/* Adds the extrema of one coordinate of a cubic Bezier.  The parameter
 * values are shared by both axes so that the actual on-curve points are
 * added, which yields the tight bounding box. */
static void
cff_cubic_param_extrema(double p0, double p1, double p2, double p3,
                        double roots[2], int *numRoots)
{
	double a = -p0 + 3.0 * p1 - 3.0 * p2 + p3;
	double b = 2.0 * (p0 - 2.0 * p1 + p2);
	double c = p1 - p0;
	double disc, sq;

	*numRoots = 0;
	if (fabs(a) < 1e-12) {
		if (fabs(b) > 1e-12) {
			roots[(*numRoots)++] = -c / b;
		}
	} else {
		disc = b * b - 4.0 * a * c;
		if (disc >= 0.0) {
			sq = sqrt(disc);
			roots[(*numRoots)++] = (-b + sq) / (2.0 * a);
			roots[(*numRoots)++] = (-b - sq) / (2.0 * a);
		}
	}
}

static void
cff_bbox_cubic(CFFBBox *box,
               double x0, double y0, double x1, double y1,
               double x2, double y2, double x3, double y3)
{
	double roots[2];
	int n, i;

	cff_bbox_add(box, x0, y0);
	cff_bbox_add(box, x3, y3);

	cff_cubic_param_extrema(x0, x1, x2, x3, roots, &n);
	for (i = 0; i < n; i++) {
		if (roots[i] > 0.0 && roots[i] < 1.0)
			cff_bbox_add(box, cff_cubic_at(x0, x1, x2, x3, roots[i]),
			             cff_cubic_at(y0, y1, y2, y3, roots[i]));
	}
	cff_cubic_param_extrema(y0, y1, y2, y3, roots, &n);
	for (i = 0; i < n; i++) {
		if (roots[i] > 0.0 && roots[i] < 1.0)
			cff_bbox_add(box, cff_cubic_at(x0, x1, x2, x3, roots[i]),
			             cff_cubic_at(y0, y1, y2, y3, roots[i]));
	}
}

/* ------------------------------------------------------------------ */
/* Type 2 charstring interpreter.                                      */

typedef struct CFFFrame
{
	uint32_t pos;
	uint32_t end;
} CFFFrame;

typedef struct CFFRun
{
	const uint8_t   *mem;
	uint32_t         limit;
	const CFFIndex  *gsubrs;
	const CFFIndex  *lsubrs;
	const SFT_CFFSink *sink;

	double           stack[CFF_MAX_STACK];
	int              sp;

	double           x, y;
	uint32_t         cur, start;
	int              open;
	int              drawn;

	uint32_t         stemCount;
	int              widthDone;

	CFFBBox          box;
} CFFRun;

static int
cff_push(CFFRun *run, double v)
{
	if (run->sp >= CFF_MAX_STACK)
		return -1;
	run->stack[run->sp++] = v;
	return 0;
}

/* Remove the lowest operand (used for the optional leading width). */
static void
cff_drop_bottom(CFFRun *run, int n)
{
	memmove(run->stack, run->stack + 1,
	        (size_t) (n - 1) * sizeof run->stack[0]);
	run->sp--;
}

static int
cff_close_contour(CFFRun *run)
{
	if (!run->open)
		return 0;
	if (run->sink && run->cur != run->start) {
		if (run->sink->line(run->sink->user, run->cur, run->start) < 0)
			return -1;
	}
	/* The start point is only guaranteed to have been added by the first
	 * segment; make sure it is covered even when the contour consists of
	 * just a moveto and a single segment. */
	if (run->cur != run->start)
		cff_bbox_add(&run->box, run->x, run->y);
	run->open = 0;
	return 0;
}

static int
cff_move_to(CFFRun *run, double nx, double ny)
{
	uint32_t idx = 0;
	if (cff_close_contour(run) < 0)
		return -1;
	run->x = nx;
	run->y = ny;
	if (run->sink &&
	    run->sink->point(run->sink->user, nx, ny, &idx) < 0)
		return -1;
	run->cur   = idx;
	run->start = idx;
	run->open  = 1;
	/* A bare moveto is not part of the drawn outline, so it must not
	 * widen the bounding box; the start point of the first real segment
	 * is added by cff_line_to() or cff_curve_to(). */
	return 0;
}

static int
cff_line_to(CFFRun *run, double nx, double ny)
{
	uint32_t idx = 0;
	double ox = run->x, oy = run->y;

	if (run->sink) {
		if (run->sink->point(run->sink->user, nx, ny, &idx) < 0)
			return -1;
		if (run->sink->line(run->sink->user, run->cur, idx) < 0)
			return -1;
		run->cur = idx;
	}
	run->x = nx;
	run->y = ny;
	if (nx != ox || ny != oy)
		run->drawn = 1;
	/* Both endpoints of a line are part of the drawn geometry. */
	cff_bbox_add(&run->box, ox, oy);
	cff_bbox_add(&run->box, nx, ny);
	return 0;
}

static int
cff_curve_to(CFFRun *run,
             double dx1, double dy1, double dx2, double dy2,
             double dx3, double dy3)
{
	double x0 = run->x, y0 = run->y;
	double x1 = x0 + dx1, y1 = y0 + dy1;
	double x2 = x1 + dx2, y2 = y1 + dy2;
	double x3 = x2 + dx3, y3 = y2 + dy3;
	uint32_t i1 = 0, i2 = 0, i3 = 0;

	cff_bbox_add(&run->box, x0, y0);
	cff_bbox_cubic(&run->box, x0, y0, x1, y1, x2, y2, x3, y3);
	if (run->sink) {
		if (run->sink->point(run->sink->user, x1, y1, &i1) < 0)
			return -1;
		if (run->sink->point(run->sink->user, x2, y2, &i2) < 0)
			return -1;
		if (run->sink->point(run->sink->user, x3, y3, &i3) < 0)
			return -1;
		if (run->sink->curve(run->sink->user, run->cur, i1, i2, i3) < 0)
			return -1;
		run->cur = i3;
	}
	run->x = x3;
	run->y = y3;
	run->drawn = 1;
	return 0;
}

/* hstem, vstem, hstemhm, vstemhm, hintmask, cntrmask.  The optional
 * width is the first operand if the count is odd.  Stem hints accumulate
 * across subroutine boundaries; the mask bytes are consumed, but never
 * interpreted. */
static int
cff_op_stems(CFFRun *run, uint32_t *pos, uint32_t end, int mask)
{
	int n = run->sp;

	if (!run->widthDone) {
		if (n & 1) {
			cff_drop_bottom(run, n);
			n--;
		}
		run->widthDone = 1;
	}
	if (n & 1)
		return -1;
	run->stemCount += (uint32_t) (n / 2);
	run->sp = 0;
	if (mask) {
		uint32_t bytes = (run->stemCount + 7) / 8;
		if (!cff_range(*pos, bytes, end))
			return -1;
		*pos += bytes;
	}
	return 0;
}

static int
cff_op_move(CFFRun *run, int kind)
{
	int need = kind == 0 ? 2 : 1;
	int n = run->sp;
	double nx, ny;

	if (!run->widthDone) {
		if (n == need + 1) {
			cff_drop_bottom(run, n);
			n--;
		}
		run->widthDone = 1;
	}
	if (n != need)
		return -1;
	if (kind == 0) {
		nx = run->x + run->stack[0];
		ny = run->y + run->stack[1];
	} else if (kind == 1) {
		nx = run->x + run->stack[0];
		ny = run->y;
	} else {
		nx = run->x;
		ny = run->y + run->stack[0];
	}
	run->sp = 0;
	return cff_move_to(run, nx, ny);
}

static int
cff_bias(uint32_t count)
{
	if (count < 1240)
		return 107;
	if (count < 33900)
		return 1131;
	return 32768;
}

/* hvcurveto (startHorizontal=1) and vhcurveto (startHorizontal=0).
 * Groups of four arguments alternate orientation; one trailing argument
 * is the final coordinate delta of the last curve. */
static int
cff_op_hv(CFFRun *run, int startHorizontal)
{
	int n = run->sp;
	int groups = n / 4;
	int trailing = (n % 4 == 1);
	int i;

	if (n < 4 || (n % 4) > 1)
		return -1;
	for (i = 0; i < groups; i++) {
		int horizontal = startHorizontal ^ (i & 1);
		const double *a = &run->stack[i * 4];
		double dx1, dy1, dx2, dy2, dx3, dy3;
		if (horizontal) {
			dx1 = a[0]; dy1 = 0.0;
			dx2 = a[1]; dy2 = a[2];
			dx3 = 0.0;  dy3 = a[3];
		} else {
			dx1 = 0.0;  dy1 = a[0];
			dx2 = a[1]; dy2 = a[2];
			dx3 = a[3]; dy3 = 0.0;
		}
		if (trailing && i == groups - 1) {
			if (horizontal)
				dx3 += run->stack[n - 1];
			else
				dy3 += run->stack[n - 1];
		}
		if (cff_curve_to(run, dx1, dy1, dx2, dy2, dx3, dy3) < 0)
			return -1;
	}
	run->sp = 0;
	run->widthDone = 1;
	return 0;
}

/* vvcurveto / hhcurveto.  An odd count means one optional leading operand
 * that applies to the first curve's first control point. */
static int
cff_op_vvhh(CFFRun *run, int vertical)
{
	int n = run->sp, idx = 0;
	double opt = 0.0;
	int first = 1;

	if (n < 4 || (n % 4) > 1)
		return -1;
	if (n % 4 == 1)
		opt = run->stack[idx++];
	for (; idx + 4 <= n; idx += 4) {
		const double *a = &run->stack[idx];
		double dx1, dy1, dx2, dy2, dx3, dy3;
		if (vertical) {
			dx1 = first ? opt : 0.0;
			dy1 = a[0];
			dx2 = a[1]; dy2 = a[2];
			dx3 = 0.0;  dy3 = a[3];
		} else {
			dx1 = a[0];
			dy1 = first ? opt : 0.0;
			dx2 = a[1]; dy2 = a[2];
			dx3 = a[3]; dy3 = 0.0;
		}
		first = 0;
		if (cff_curve_to(run, dx1, dy1, dx2, dy2, dx3, dy3) < 0)
			return -1;
	}
	run->sp = 0;
	run->widthDone = 1;
	return 0;
}

/* The four flex operators. */
static int
cff_op_flex(CFFRun *run, int kind)
{
	const double *a = run->stack;
	int n = run->sp, need;

	switch (kind) {
	case 34: need = 7;  break; /* hflex  */
	case 35: need = 13; break; /* flex   */
	case 36: need = 9;  break; /* hflex1 */
	case 37: need = 11; break; /* flex1  */
	default: return -1;
	}
	if (n != need)
		return -1;
	if (kind == 34) { /* hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6 */
		double dy2 = a[2];
		if (cff_curve_to(run, a[0], 0.0, a[1], dy2, a[3], 0.0) < 0)
			return -1;
		if (cff_curve_to(run, a[4], 0.0, a[5], -dy2, a[6], 0.0) < 0)
			return -1;
	} else if (kind == 35) { /* flex: two explicit curves + depth */
		if (cff_curve_to(run, a[0], a[1], a[2], a[3], a[4], a[5]) < 0)
			return -1;
		if (cff_curve_to(run, a[6], a[7], a[8], a[9], a[10], a[11]) < 0)
			return -1;
	} else if (kind == 36) { /* hflex1 */
		double dy1 = a[1], dy2 = a[3], dy5 = a[7];
		if (cff_curve_to(run, a[0], dy1, a[2], dy2, a[4], 0.0) < 0)
			return -1;
		if (cff_curve_to(run, a[5], 0.0, a[6], dy5, a[8],
		                 -(dy1 + dy2 + dy5)) < 0)
			return -1;
	} else { /* flex1: dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 d6 */
		double dx1 = a[0], dy1 = a[1], dx2 = a[2], dy2 = a[3];
		double dx3 = a[4], dy3 = a[5], dx4 = a[6], dy4 = a[7];
		double dx5 = a[8], dy5 = a[9], d6 = a[10];
		double sumdx = dx1 + dx2 + dx3 + dx4 + dx5;
		double sumdy = dy1 + dy2 + dy3 + dy4 + dy5;
		double dx6, dy6;
		if (cff_curve_to(run, dx1, dy1, dx2, dy2, dx3, dy3) < 0)
			return -1;
		if (fabs(sumdx) > fabs(sumdy)) {
			dx6 = d6;
			dy6 = -sumdy;
		} else {
			dx6 = -sumdx;
			dy6 = d6;
		}
		if (cff_curve_to(run, dx4, dy4, dx5, dy5, dx6, dy6) < 0)
			return -1;
	}
	run->sp = 0;
	run->widthDone = 1;
	return 0;
}

/* Draws a subroutine, enforcing the Type 2 nesting limit. */
static int
cff_call_subr(CFFRun *run, CFFFrame *frames, uint32_t *depth,
              uint32_t *pos, uint32_t *end, const CFFIndex *subrs)
{
	double v;
	int bias;
	uint32_t idx, s, e;
	int count;

	if (run->sp < 1 || subrs->count == 0)
		return -1;
	v = run->stack[--run->sp];
	count = (int) subrs->count;
	bias = cff_bias(subrs->count);
	{
		double t = v + (double) bias;
		if (!(t >= 0.0 && t < (double) count))
			return -1;
		if ((double) (uint32_t) t != t)
			return -1;
		idx = (uint32_t) t;
	}
	if (cff_index_item(run->mem, subrs, idx, &s, &e) < 0)
		return -1;
	if (*depth >= CFF_MAX_SUBR_DEPTH)
		return -1;
	frames[*depth].pos = *pos;
	frames[*depth].end = *end;
	(*depth)++;
	*pos = s;
	*end = e;
	return 0;
}

static int
cff_run_program(CFFRun *run, uint32_t start, uint32_t end)
{
	CFFFrame frames[CFF_MAX_SUBR_DEPTH];
	uint32_t pos = start;
	uint32_t depth = 0;
	uint8_t b, e;
	int n, i;

	for (;;) {
		if (pos >= end) {
			/* Unterminated program, or a subroutine without `return`. */
			return -1;
		}
		b = run->mem[pos++];
		if (b >= 32 && b <= 246) {
			if (cff_push(run, (double) b - 139.0) < 0)
				return -1;
			continue;
		}
		if (b >= 247 && b <= 250) {
			if (!cff_range(pos, 1, end))
				return -1;
			if (cff_push(run, (double) ((b - 247) * 256 +
			                           run->mem[pos++] + 108)) < 0)
				return -1;
			continue;
		}
		if (b >= 251 && b <= 254) {
			if (!cff_range(pos, 1, end))
				return -1;
			if (cff_push(run, -(double) ((b - 251) * 256 +
			                            run->mem[pos++] + 108)) < 0)
				return -1;
			continue;
		}
		if (b == 28) {
			int16_t v;
			if (cff_i16(run->mem, end, pos, &v) < 0)
				return -1;
			pos += 2;
			if (cff_push(run, v) < 0)
				return -1;
			continue;
		}
		if (b == 255) {
			int32_t v;
			if (cff_i32(run->mem, end, pos, &v) < 0)
				return -1;
			pos += 4;
			if (cff_push(run, (double) v / 65536.0) < 0)
				return -1;
			continue;
		}

		switch (b) {
		case 1:   /* hstem */
		case 3:   /* vstem */
		case 18:  /* hstemhm */
		case 23:  /* vstemhm */
			if (cff_op_stems(run, &pos, end, 0) < 0)
				return -1;
			break;
		case 19:  /* hintmask */
		case 20:  /* cntrmask */
			if (cff_op_stems(run, &pos, end, 1) < 0)
				return -1;
			break;
		case 21:  /* rmoveto */
			if (cff_op_move(run, 0) < 0)
				return -1;
			break;
		case 22:  /* hmoveto */
			if (cff_op_move(run, 1) < 0)
				return -1;
			break;
		case 4:   /* vmoveto */
			if (cff_op_move(run, 2) < 0)
				return -1;
			break;
		case 5:   /* rlineto */
			n = run->sp;
			if (n < 2 || (n & 1))
				return -1;
			for (i = 0; i < n; i += 2) {
				if (cff_line_to(run, run->x + run->stack[i],
				                run->y + run->stack[i + 1]) < 0)
					return -1;
			}
			run->sp = 0;
			run->widthDone = 1;
			break;
		case 6:   /* hlineto */
		case 7:   /* vlineto */
			n = run->sp;
			if (n < 1)
				return -1;
			{
				int horizontal = b == 6;
				for (i = 0; i < n; i++) {
					if (horizontal) {
						if (cff_line_to(run, run->x + run->stack[i],
						                run->y) < 0)
							return -1;
					} else {
						if (cff_line_to(run, run->x,
						                run->y + run->stack[i]) < 0)
							return -1;
					}
					horizontal = !horizontal;
				}
			}
			run->sp = 0;
			run->widthDone = 1;
			break;
		case 8:   /* rrcurveto */
			n = run->sp;
			if (n < 6 || n % 6)
				return -1;
			for (i = 0; i < n; i += 6) {
				if (cff_curve_to(run, run->stack[i], run->stack[i + 1],
				                 run->stack[i + 2], run->stack[i + 3],
				                 run->stack[i + 4], run->stack[i + 5]) < 0)
					return -1;
			}
			run->sp = 0;
			run->widthDone = 1;
			break;
		case 24:  /* rcurveline */
			n = run->sp;
			if (n < 8 || (n - 2) % 6)
				return -1;
			for (i = 0; i + 6 <= n - 2; i += 6) {
				if (cff_curve_to(run, run->stack[i], run->stack[i + 1],
				                 run->stack[i + 2], run->stack[i + 3],
				                 run->stack[i + 4], run->stack[i + 5]) < 0)
					return -1;
			}
			if (cff_line_to(run, run->x + run->stack[n - 2],
			                run->y + run->stack[n - 1]) < 0)
				return -1;
			run->sp = 0;
			run->widthDone = 1;
			break;
		case 25:  /* rlinecurve */
			n = run->sp;
			if (n < 8 || (n - 6) % 2)
				return -1;
			for (i = 0; i < n - 6; i += 2) {
				if (cff_line_to(run, run->x + run->stack[i],
				                run->y + run->stack[i + 1]) < 0)
					return -1;
			}
			if (cff_curve_to(run, run->stack[n - 6], run->stack[n - 5],
			                 run->stack[n - 4], run->stack[n - 3],
			                 run->stack[n - 2], run->stack[n - 1]) < 0)
				return -1;
			run->sp = 0;
			run->widthDone = 1;
			break;
		case 26:  /* vvcurveto */
			if (cff_op_vvhh(run, 1) < 0)
				return -1;
			break;
		case 27:  /* hhcurveto */
			if (cff_op_vvhh(run, 0) < 0)
				return -1;
			break;
		case 30:  /* vhcurveto */
			if (cff_op_hv(run, 0) < 0)
				return -1;
			break;
		case 31:  /* hvcurveto */
			if (cff_op_hv(run, 1) < 0)
				return -1;
			break;
		case 10:  /* callsubr */
			if (cff_call_subr(run, frames, &depth, &pos, &end,
			                  run->lsubrs) < 0)
				return -1;
			break;
		case 29:  /* callgsubr */
			if (cff_call_subr(run, frames, &depth, &pos, &end,
			                  run->gsubrs) < 0)
				return -1;
			break;
		case 11:  /* return */
			if (depth == 0)
				return -1;
			depth--;
			pos = frames[depth].pos;
			end = frames[depth].end;
			break;
		case 14:  /* endchar */
			n = run->sp;
			if (!run->widthDone) {
				if (n == 1) {
					n = 0;
				} else if (n == 4 || n == 5) {
					return -1; /* seac is not supported */
				} else if (n != 0) {
					return -1;
				}
				run->widthDone = 1;
			} else if (n != 0) {
				return -1; /* includes the deprecated seac form */
			}
			if (cff_close_contour(run) < 0)
				return -1;
			return 0;
		case 12:  /* escape */
			if (!cff_range(pos, 1, end))
				return -1;
			e = run->mem[pos++];
			switch (e) {
			case 0: /* dotsection: obsolete no-op */
				run->sp = 0;
				break;
			case 34:
			case 35:
			case 36:
			case 37:
				if (cff_op_flex(run, e) < 0)
					return -1;
				break;
			default:
				/* Arithmetic, storage, random, conditionals and
				 * callothersubr are deliberately unsupported. */
				return -1;
			}
			break;
		default:
			/* Reserved / Type 1 operators are not valid Type 2. */
			return -1;
		}
	}
}

int
sfti_cff_decode(const uint8_t *memory, uint32_t memorySize,
                const SFT_CFF *cff, uint32_t glyph,
                const SFT_CFFSink *sink, double bbox[4], int *hasOutline)
{
	CFFIndex charStrings, gsubrs, lsubrs;
	CFFRun run;
	uint32_t limit, start, end;
	int result;

	*hasOutline = 0;
	bbox[0] = bbox[1] = bbox[2] = bbox[3] = 0.0;

	if (!cff || cff->base > memorySize || cff->size > memorySize - cff->base)
		return -1;
	limit = cff->base + cff->size;

	if (cff_index_read(memory, limit, cff->charStrings, &charStrings) < 0)
		return -1;
	if (glyph >= charStrings.count)
		return -1;
	if (cff_index_item(memory, &charStrings, glyph, &start, &end) < 0)
		return -1;

	if (cff_index_read(memory, limit, cff->globalSubrs, &gsubrs) < 0)
		return -1;
	memset(&lsubrs, 0, sizeof lsubrs);
	if (cff->localSubrs != 0) {
		if (cff_index_read(memory, limit, cff->localSubrs, &lsubrs) < 0)
			return -1;
	}

	memset(&run, 0, sizeof run);
	run.mem    = memory;
	run.limit  = limit;
	run.gsubrs = &gsubrs;
	run.lsubrs = &lsubrs;
	run.sink   = sink;

	result = cff_run_program(&run, start, end);
	if (result < 0)
		return -1;

	if (run.drawn && run.box.any) {
		*hasOutline = 1;
		bbox[0] = run.box.minX;
		bbox[1] = run.box.minY;
		bbox[2] = run.box.maxX;
		bbox[3] = run.box.maxY;
	}
	return 0;
}
