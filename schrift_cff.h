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

#ifndef SCHRIFT_CFF_H
#define SCHRIFT_CFF_H 1

#include <stddef.h>
#include <stdint.h>

/* Everything in this header is private to libschrift.  The public ABI lives
 * in schrift.h and must not depend on these types. */

/* Pre-parsed, immutable information about the single CFF font in a file.
 * All offsets are absolute byte offsets into the font's memory.  A zero
 * offset means that the corresponding structure is absent. */
typedef struct SFT_CFF
{
	uint32_t base;             /* start of the 'CFF ' table */
	uint32_t size;             /* length of the 'CFF ' table */

	uint32_t charStrings;      /* start of the CharStrings INDEX */
	uint32_t charStringsCount;

	uint32_t globalSubrs;      /* start of the Global Subrs INDEX */
	uint32_t globalSubrsCount;

	uint32_t localSubrs;       /* start of the Local Subrs INDEX */
	uint32_t localSubrsCount;

	uint32_t privateOffset;    /* start of the Private DICT */
	uint32_t privateSize;

	int32_t  defaultWidthX;
	int32_t  nominalWidthX;
	int      charstringType;
} SFT_CFF;

/* A sink for the outline produced by the Type 2 interpreter.  Points have
 * already been expressed in font units; the interpreter does not apply any
 * scaling.  Every function returns 0 on success and -1 on failure. */
typedef struct SFT_CFFSink
{
	void *user;
	int (*point)(void *user, double x, double y, uint32_t *index);
	int (*line)(void *user, uint32_t begin, uint32_t end);
	int (*curve)(void *user, uint32_t begin, uint32_t control1,
	             uint32_t control2, uint32_t end);
} SFT_CFFSink;

/* Validates the CFF table and fills out `cff`.  `numGlyphs`, when non-zero,
 * must match the number of CharStrings.  Returns 0 or -1. */
int sfti_cff_init(const uint8_t *memory, uint32_t memorySize,
                  uint32_t tableOffset, uint32_t tableSize,
                  uint32_t numGlyphs, SFT_CFF *cff);

/* Interprets one glyph.  If `sink` is non-NULL the outline is delivered
 * through it.  `bbox` receives the tight bounding box of the actual curves
 * (not of the control polygon) and `hasOutline` tells whether the glyph
 * contains any drawable segment with non-zero extent.  Returns 0 or -1. */
int sfti_cff_decode(const uint8_t *memory, uint32_t memorySize,
                    const SFT_CFF *cff, uint32_t glyph,
                    const SFT_CFFSink *sink, double bbox[4],
                    int *hasOutline);

#endif
