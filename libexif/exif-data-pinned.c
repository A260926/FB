/* exif-data-pinned.c
 *
 * Pinned MakerNote save entry point.
 *
 * Unlike exif_data_save_data(), this writer never moves a MakerNote block
 * that was read from valid EXIF input: the block is copied byte for byte at
 * its original TIFF-relative offset, so absolute offsets stored inside the
 * opaque vendor data keep pointing at the same bytes. The remaining TIFF
 * structure (directories, external value areas, thumbnail) is rebuilt from
 * the current ExifData and packed into the free space in front of and behind
 * the fixed block.
 *
 * Only the five standard libexif IFDs and their standard connections are
 * emitted. Unknown MakerNote references that point outside the pinned block
 * are neither parsed nor relocated.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <config.h>

#include <libexif/exif-data.h>
#include <libexif/exif-content.h>
#include <libexif/exif-entry.h>
#include <libexif/exif-format.h>
#include <libexif/exif-ifd.h>
#include <libexif/exif-mem.h>
#include <libexif/exif-tag.h>
#include <libexif/exif-utils.h>
#include "exif-data-pinned-priv.h"

#include <string.h>

/* Largest output this entry point produces. The 16 bit JPEG APP1 length
 * field caps the segment at 65533 bytes of EXIF payload (65535 bytes of
 * segment minus the two length bytes). */
#define PINNED_MAX_SIZE 65533u

/* TIFF header occupies the first 8 bytes of the TIFF block (after the six
 * byte "Exif\0\0" marker). */
#define PINNED_TIFF_HEADER_SIZE 8u

/* Block kinds. */
#define PINNED_KIND_DIRECTORY 0
#define PINNED_KIND_VALUE     1
#define PINNED_KIND_NOTE      2

/* A relocatable block: an IFD directory (entry count, tag entries and the
 * next-IFD long), an external value area (>4 byte entry data or the
 * thumbnail), or the fixed MakerNote. */
typedef struct _PinnedBlock PinnedBlock;
struct _PinnedBlock {
	unsigned int size;	/* padded to an even length */
	unsigned int raw_size;	/* unpadded length */
	unsigned int offset;	/* TIFF-relative offset */
	unsigned char kind;
};

/* One entry record kept while the layout is computed. */
typedef struct _PinnedEntry PinnedEntry;
struct _PinnedEntry {
	ExifEntry *entry;	/* NULL for the synthesized pointer entries */
	ExifTag tag;
	ExifFormat format;
	unsigned long components;
	unsigned int value_size;	/* format size * components, unpadded */
	unsigned int block;		/* value block index, ~0 if inline */
};

/* One directory record kept while the layout is computed. */
typedef struct _PinnedDir PinnedDir;
struct _PinnedDir {
	PinnedEntry *entries;
	unsigned int count;
	unsigned int block;		/* directory block index, ~0 if unused */
	unsigned int next_ifd;		/* block index of the chained IFD, ~0 */
};

typedef struct _PinnedLayout PinnedLayout;
struct _PinnedLayout {
	ExifData *data;
	ExifMem *mem;
	ExifByteOrder order;

	PinnedBlock *blocks;
	unsigned int nblocks;
	unsigned int note_block;

	PinnedDir dirs[EXIF_IFD_COUNT];

	PinnedNote note;
	int has_note;

	/* Bounds of the fixed MakerNote inside the TIFF block. */
	unsigned int note_start;
	unsigned int note_end;

	unsigned char *out;
	unsigned int size;
};

/* -------------------------------------------------------------- */
/* Layout construction: enumerate directories, entries and blocks */
/* -------------------------------------------------------------- */

static unsigned int
pinned_even (unsigned int n)
{
	return (n + 1u) & ~1u;
}

static int
pinned_add_block (PinnedLayout *l, unsigned int raw_size, unsigned char kind)
{
	PinnedBlock *t;
	unsigned int n = l->nblocks + 1;

	t = exif_mem_realloc (l->mem, l->blocks,
			      (unsigned long) n * sizeof (PinnedBlock));
	if (!t)
		return -1;
	l->blocks = t;
	l->blocks[l->nblocks].size = pinned_even (raw_size);
	l->blocks[l->nblocks].raw_size = raw_size;
	l->blocks[l->nblocks].offset = 0;
	l->blocks[l->nblocks].kind = kind;
	l->nblocks++;
	return (int) (l->nblocks - 1);
}

/* Validate one entry and register an external value block if needed. */
static int
pinned_prepare_entry (PinnedLayout *l, ExifEntry *e, PinnedEntry *pe)
{
	unsigned char fs;

	pe->entry = e;
	pe->tag = e->tag;
	pe->format = e->format;
	pe->components = e->components;
	pe->block = ~0u;

	fs = exif_format_get_size (e->format);
	if (!fs || e->components == 0 ||
	    e->components > 0xffffffffUL / fs)
		return -1;
	pe->value_size =
		(unsigned int) (e->components * (unsigned long) fs);

	/* The entry may carry an oversized buffer but never an undersized
	 * one: writing the declared value must not read past it. */
	if (e->size < pe->value_size)
		return -1;

	if (pe->value_size > 4) {
		int b;

		if (e->tag == EXIF_TAG_MAKER_NOTE) {
			/* The pinned block itself is the value area. */
			if (!l->has_note)
				return -1;
			pe->block = l->note_block;
			return 0;
		}
		b = pinned_add_block (l, pe->value_size, PINNED_KIND_VALUE);
		if (b < 0)
			return -1;
		pe->block = (unsigned int) b;
	}
	return 0;
}

static int
pinned_build (PinnedLayout *l, unsigned int capacity)
{
	ExifData *d = l->data;
	unsigned int i, j;
	int pin_state;

	l->has_note = 0;
	l->note_block = ~0u;
	memset (l->dirs, 0, sizeof (l->dirs));
	for (i = 0; i < EXIF_IFD_COUNT; i++) {
		l->dirs[i].block = ~0u;
		l->dirs[i].next_ifd = ~0u;
	}

	/* Resolve the pinned MakerNote. */
	pin_state = exif_data_pinned_get (d, &l->note);
	if (pin_state < 0)
		return -1;	/* position cannot be proven: do not guess */
	if (pin_state > 0 && l->note.size > 4 &&
	    l->note.offset >= PINNED_TIFF_HEADER_SIZE) {
		ExifEntry *mn = exif_content_get_entry (
			d->ifd[EXIF_IFD_EXIF], EXIF_TAG_MAKER_NOTE);
		unsigned char fs;
		int b;

		if (!mn || !mn->data)
			return -1;	/* deleted and not re-created identically */
		if (mn->size != l->note.size ||
		    memcmp (mn->data, l->note.data, l->note.size) != 0)
			return -1;	/* modified or rebuilt from scratch */
		if (l->note.order != l->order)
			return -1;	/* byte order switched */
		fs = exif_format_get_size (mn->format);
		if (!fs || mn->components == 0 ||
		    mn->components > 0xffffffffUL / fs ||
		    (unsigned int) (mn->components *
				    (unsigned long) fs) != l->note.size)
			return -1;	/* entry geometry no longer matches */
		if (l->note.offset > capacity ||
		    l->note.size > capacity - l->note.offset)
			return -1;

		l->has_note = 1;
		l->note_start = l->note.offset;
		l->note_end = pinned_even (l->note.offset + l->note.size);
		if (l->note_end > capacity)
			return -1;

		b = pinned_add_block (l, l->note.size, PINNED_KIND_NOTE);
		if (b < 0)
			return -1;
		l->note_block = (unsigned int) b;
		l->blocks[l->note_block].offset = l->note.offset;
	}
	if (!l->has_note && pin_state == 0 &&
	    exif_content_get_entry (d->ifd[EXIF_IFD_EXIF],
				    EXIF_TAG_MAKER_NOTE)) {
		/* A MakerNote exists without any recorded origin: never
		 * guess its format or rebuild it through a vendor
		 * interpreter. A pin_state > 0 note that was inline
		 * (<=4 bytes, or not external in the source) has no
		 * internal addresses and falls through to be saved as an
		 * ordinary entry. */
		return -1;
	}

	/* Build the entry lists. The directory chains are the standard
	 * libexif connections:
	 *   IFD0 -> {EXIF, GPS}, IFD0 -> IFD1, EXIF -> INTEROP.
	 */
	for (i = 0; i < EXIF_IFD_COUNT; i++) {
		ExifContent *c = d->ifd[i];
		unsigned int extra = 0;
		unsigned int n;
		PinnedEntry *pes;

		switch (i) {
		case EXIF_IFD_0:
			if (d->ifd[EXIF_IFD_EXIF]->count ||
			    d->ifd[EXIF_IFD_INTEROPERABILITY]->count)
				extra++;
			if (d->ifd[EXIF_IFD_GPS]->count)
				extra++;
			break;
		case EXIF_IFD_EXIF:
			if (d->ifd[EXIF_IFD_INTEROPERABILITY]->count)
				extra++;
			break;
		case EXIF_IFD_1:
			if (d->size)
				extra += 2;
			break;
		default:
			break;
		}

		n = c->count + extra;
		l->dirs[i].count = n;
		if (n) {
			pes = exif_mem_alloc (l->mem,
				((unsigned long) n) * sizeof (PinnedEntry));
			if (!pes)
				return -1;
			l->dirs[i].entries = pes;
		}

		for (j = 0; j < c->count; j++) {
			ExifEntry *e = c->entries[j];
			if (!e)
				return -1;
			if (pinned_prepare_entry (l, e, &pes[j]) < 0)
				return -1;
		}

		/* Synthesized entries. */
		j = c->count;
		switch (i) {
		case EXIF_IFD_0:
			if (d->ifd[EXIF_IFD_EXIF]->count ||
			    d->ifd[EXIF_IFD_INTEROPERABILITY]->count) {
				pes[j].entry = NULL;
				pes[j].tag = EXIF_TAG_EXIF_IFD_POINTER;
				pes[j].format = EXIF_FORMAT_LONG;
				pes[j].components = 1;
				pes[j].value_size = 4;
				pes[j].block = ~0u;
				j++;
			}
			if (d->ifd[EXIF_IFD_GPS]->count) {
				pes[j].entry = NULL;
				pes[j].tag = EXIF_TAG_GPS_INFO_IFD_POINTER;
				pes[j].format = EXIF_FORMAT_LONG;
				pes[j].components = 1;
				pes[j].value_size = 4;
				pes[j].block = ~0u;
				j++;
			}
			break;
		case EXIF_IFD_EXIF:
			if (d->ifd[EXIF_IFD_INTEROPERABILITY]->count) {
				pes[j].entry = NULL;
				pes[j].tag =
					EXIF_TAG_INTEROPERABILITY_IFD_POINTER;
				pes[j].format = EXIF_FORMAT_LONG;
				pes[j].components = 1;
				pes[j].value_size = 4;
				pes[j].block = ~0u;
				j++;
			}
			break;
		case EXIF_IFD_1:
			if (d->size) {
				int tb = pinned_add_block (l, d->size,
							   PINNED_KIND_VALUE);
				if (tb < 0)
					return -1;
				pes[j].entry = NULL;
				pes[j].tag = EXIF_TAG_JPEG_INTERCHANGE_FORMAT;
				pes[j].format = EXIF_FORMAT_LONG;
				pes[j].components = 1;
				pes[j].value_size = 4;
				pes[j].block = (unsigned int) tb;
				j++;
				pes[j].entry = NULL;
				pes[j].tag =
					EXIF_TAG_JPEG_INTERCHANGE_FORMAT_LENGTH;
				pes[j].format = EXIF_FORMAT_LONG;
				pes[j].components = 1;
				pes[j].value_size = 4;
				pes[j].block = ~0u;
				j++;
			}
			break;
		default:
			break;
		}
	}

	/* Directory blocks. IFD0 is always emitted, even when empty. */
	for (i = 0; i < EXIF_IFD_COUNT; i++) {
		unsigned int raw;
		int b;

		if (i != EXIF_IFD_0 && !l->dirs[i].count)
			continue;
		raw = 2u + 12u * l->dirs[i].count + 4u;
		b = pinned_add_block (l, raw, PINNED_KIND_DIRECTORY);
		if (b < 0)
			return -1;
		l->dirs[i].block = (unsigned int) b;
	}

	/* Standard directory links. */
	if (l->dirs[EXIF_IFD_1].count || d->size)
		l->dirs[EXIF_IFD_0].next_ifd = l->dirs[EXIF_IFD_1].block;

	return 0;
}

/* ----------------------------------------------------------------------- */
/* Exact two-space packing. Movable blocks go before or after the pinned    */
/* MakerNote; every block keeps an even start and an even padded length.    */
/* ----------------------------------------------------------------------- */

/* Exhaustive subset enumeration for small block sets: every partition is a
 * genuine feasibility witness, never a heuristic. */
static int
pinned_pack_exhaustive (PinnedLayout *l,
			unsigned int front_room, unsigned int back_room)
{
	PinnedBlock *blocks = l->blocks;
	unsigned int n = l->nblocks;
	unsigned long combos = 1UL << n;
	unsigned long mask;
	unsigned int i;

	for (mask = 0; mask < combos; mask++) {
		unsigned int front_sum = 0, back_sum = 0;
		unsigned int front_pos, back_pos;

		for (i = 0; i < n; i++) {
			if (blocks[i].kind == PINNED_KIND_NOTE)
				continue;
			if (mask & (1UL << i))
				back_sum += blocks[i].size;
			else
				front_sum += blocks[i].size;
		}
		if (front_sum > front_room || back_sum > back_room)
			continue;

		front_pos = PINNED_TIFF_HEADER_SIZE;
		back_pos = l->has_note ? l->note_end : 0;
		for (i = 0; i < n; i++) {
			if (blocks[i].kind == PINNED_KIND_NOTE)
				continue;
			if (mask & (1UL << i)) {
				blocks[i].offset = back_pos;
				back_pos += blocks[i].size;
			} else {
				blocks[i].offset = front_pos;
				front_pos += blocks[i].size;
			}
		}
		l->size = l->has_note
			? MAX (back_pos, l->note_end)
			: front_pos;
		return 0;
	}
	return -1;
}

/* Exact subset-sum based partition for larger block sets. dp[s] records the
 * block that first achieved front-region sum s; backtracking yields one
 * concrete partition whenever one exists. */
static int
pinned_pack_dp (PinnedLayout *l,
		unsigned int front_room, unsigned int back_room)
{
	PinnedBlock *blocks = l->blocks;
	unsigned int n = l->nblocks;
	unsigned int total = 0, target_min, s, i;
	int *dp;			/* -1 = unreachable */
	unsigned char *front_side;
	unsigned int chosen = ~0u;

	for (i = 0; i < n; i++) {
		if (blocks[i].kind == PINNED_KIND_NOTE)
			continue;
		total += blocks[i].size;
	}
	if (total > front_room + back_room)
		return -1;
	target_min = (total > back_room) ? (total - back_room) : 0;
	if (target_min > front_room)
		return -1;

	dp = exif_mem_alloc (l->mem,
			     ((unsigned long) front_room + 1u) * sizeof (int));
	if (!dp)
		return -1;
	front_side = exif_mem_alloc (l->mem, n);
	if (!front_side) {
		exif_mem_free (l->mem, dp);
		return -1;
	}
	for (s = 0; s <= front_room; s++)
		dp[s] = -1;
	dp[0] = -2;			/* reachable seed */

	for (i = 0; i < n; i++) {
		unsigned int w = blocks[i].size;
		if (blocks[i].kind == PINNED_KIND_NOTE)
			continue;
		for (s = front_room; s >= w; s--) {
			if (dp[s] == -1 && dp[s - w] != -1)
				dp[s] = (int) i;
		}
	}

	for (s = target_min; s <= front_room; s++) {
		if (dp[s] != -1) {
			chosen = s;
			break;
		}
	}
	if (chosen == ~0u) {
		exif_mem_free (l->mem, front_side);
		exif_mem_free (l->mem, dp);
		return -1;
	}

	/* Backtrack: blocks on the chain go to the front, the rest back. */
	s = chosen;
	while (s > 0) {
		int b = dp[s];
		if (b < 0)
			break;
		front_side[b] = 1;
		s -= blocks[b].size;
	}
	exif_mem_free (l->mem, dp);

	{
		unsigned int front_pos = PINNED_TIFF_HEADER_SIZE;
		unsigned int back_pos = l->has_note ? l->note_end : 0;
		for (i = 0; i < n; i++) {
			if (blocks[i].kind == PINNED_KIND_NOTE)
				continue;
			if (front_side[i]) {
				blocks[i].offset = front_pos;
				front_pos += blocks[i].size;
			} else {
				blocks[i].offset = back_pos;
				back_pos += blocks[i].size;
			}
		}
		l->size = l->has_note
			? MAX (back_pos, l->note_end)
			: front_pos;
	}
	exif_mem_free (l->mem, front_side);
	return 0;
}

static int
pinned_pack (PinnedLayout *l, unsigned int capacity)
{
	unsigned int front_room, back_room;

	if (l->has_note) {
		front_room = l->note_start - PINNED_TIFF_HEADER_SIZE;
		back_room = capacity - l->note_end;
	} else {
		front_room = capacity - PINNED_TIFF_HEADER_SIZE;
		back_room = 0;
	}

	if (l->nblocks <= 20)
		return pinned_pack_exhaustive (l, front_room, back_room);
	return pinned_pack_dp (l, front_room, back_room);
}

/* -------------------------------------------------------------- */
/* Emission: write the arranged blocks and wire every pointer up  */
/* -------------------------------------------------------------- */

static int
pinned_tag_cmp (const void *a, const void *b)
{
	ExifTag ta = ((const PinnedEntry *) a)->tag;
	ExifTag tb = ((const PinnedEntry *) b)->tag;

	return (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
}

static int
pinned_emit (PinnedLayout *l)
{
	ExifData *d = l->data;
	ExifByteOrder o = l->order;
	unsigned char *out;
	unsigned char *tiff;
	unsigned int i, j;
	PinnedEntry *sorted;
	unsigned int max_entries = 0;

	if (l->size > PINNED_MAX_SIZE - 6u)
		return -1;

	for (i = 0; i < EXIF_IFD_COUNT; i++)
		if (l->dirs[i].count > max_entries)
			max_entries = l->dirs[i].count;
	sorted = exif_mem_alloc (l->mem,
				 ((unsigned long)
				  (max_entries ? max_entries : 1)) *
				 sizeof (PinnedEntry));
	if (!sorted)
		return -1;

	out = exif_mem_alloc (l->mem, 6u + l->size);
	if (!out) {
		exif_mem_free (l->mem, sorted);
		return -1;
	}
	l->out = out;

	memcpy (out, "Exif\0\0", 6);
	tiff = out + 6;

	/* TIFF header. */
	memcpy (tiff, (o == EXIF_BYTE_ORDER_INTEL) ? "II" : "MM", 2);
	exif_set_short (tiff + 2, o, 0x002a);
	exif_set_long (tiff + 4, o,
		       l->blocks[l->dirs[EXIF_IFD_0].block].offset);

	/* Fixed MakerNote, byte for byte at its original TIFF offset. */
	if (l->has_note)
		memcpy (tiff + l->note.offset, l->note.data, l->note.size);

	/* External value data. The MakerNote value area is the pinned block
	 * itself and must not be overwritten; the thumbnail is copied from
	 * ExifData directly. */
	for (i = 0; i < EXIF_IFD_COUNT; i++) {
		PinnedDir *pd = &l->dirs[i];
		for (j = 0; j < pd->count; j++) {
			PinnedEntry *pe = &pd->entries[j];
			if (!pe->entry || pe->value_size <= 4)
				continue;
			if (pe->tag == EXIF_TAG_MAKER_NOTE)
				continue;
			memcpy (tiff + l->blocks[pe->block].offset,
				pe->entry->data, pe->value_size);
		}
	}
	if (d->size) {
		PinnedDir *pd = &l->dirs[EXIF_IFD_1];
		unsigned int tb = ~0u;
		for (j = 0; j < pd->count; j++) {
			if (pd->entries[j].tag ==
			    EXIF_TAG_JPEG_INTERCHANGE_FORMAT) {
				tb = pd->entries[j].block;
				break;
			}
		}
		if (tb == ~0u) {
			exif_mem_free (l->mem, sorted);
			return -1;
		}
		memcpy (tiff + l->blocks[tb].offset, d->data, d->size);
	}

	/* Directories, tag sorted with all pointers resolved. */
	for (i = 0; i < EXIF_IFD_COUNT; i++) {
		PinnedDir *pd = &l->dirs[i];
		unsigned int dir_pos, next, k;

		if (i != EXIF_IFD_0 && !pd->count)
			continue;
		memcpy (sorted, pd->entries,
			pd->count * sizeof (PinnedEntry));
		qsort (sorted, pd->count, sizeof (PinnedEntry), pinned_tag_cmp);

		dir_pos = l->blocks[pd->block].offset;
		exif_set_short (tiff + dir_pos, o, (ExifShort) pd->count);
		for (k = 0; k < pd->count; k++) {
			PinnedEntry *pe = &sorted[k];
			unsigned int epos = dir_pos + 2 + 12 * k;

			exif_set_short (tiff + epos + 0, o,
					(ExifShort) pe->tag);
			exif_set_short (tiff + epos + 2, o,
					(ExifShort) pe->format);
			exif_set_long (tiff + epos + 4, o,
				       (ExifLong) pe->components);
			memset (tiff + epos + 8, 0, 4);

			if (pe->tag == EXIF_TAG_EXIF_IFD_POINTER) {
				exif_set_long (tiff + epos + 8, o,
					l->blocks[l->dirs[EXIF_IFD_EXIF].block].offset);
			} else if (pe->tag == EXIF_TAG_GPS_INFO_IFD_POINTER) {
				exif_set_long (tiff + epos + 8, o,
					l->blocks[l->dirs[EXIF_IFD_GPS].block].offset);
			} else if (pe->tag ==
				   EXIF_TAG_INTEROPERABILITY_IFD_POINTER) {
				exif_set_long (tiff + epos + 8, o,
					l->blocks[l->dirs[EXIF_IFD_INTEROPERABILITY].block].offset);
			} else if (pe->tag ==
				   EXIF_TAG_JPEG_INTERCHANGE_FORMAT) {
				exif_set_long (tiff + epos + 8, o,
					       l->blocks[pe->block].offset);
			} else if (pe->tag ==
				   EXIF_TAG_JPEG_INTERCHANGE_FORMAT_LENGTH) {
				exif_set_long (tiff + epos + 8, o, d->size);
			} else if (pe->value_size <= 4) {
				/* Inline scalar: copied verbatim, unused
				 * inline bytes stay zero. */
				if (pe->entry && pe->entry->data)
					memcpy (tiff + epos + 8,
						pe->entry->data, pe->value_size);
			} else {
				exif_set_long (tiff + epos + 8, o,
					       l->blocks[pe->block].offset);
			}
		}
		next = (pd->next_ifd == ~0u) ? 0 :
		       l->blocks[pd->next_ifd].offset;
		exif_set_long (tiff + dir_pos + 2 + 12 * pd->count, o, next);
	}

	exif_mem_free (l->mem, sorted);
	return 0;
}

static void
pinned_layout_free (PinnedLayout *l)
{
	unsigned int i;

	for (i = 0; i < EXIF_IFD_COUNT; i++) {
		if (l->dirs[i].entries)
			exif_mem_free (l->mem, l->dirs[i].entries);
	}
	if (l->blocks)
		exif_mem_free (l->mem, l->blocks);
	if (l->out)
		exif_mem_free (l->mem, l->out);
}

/* -------------------------------------------------------------- */
/* Public entry point                                             */
/* -------------------------------------------------------------- */

int
exif_data_save_data_pinned (ExifData *data, unsigned int max_size,
			    unsigned char **out, unsigned int *size)
{
	PinnedLayout l;
	int rc;

	if (out)
		*out = NULL;
	if (size)
		*size = 0;
	if (!data || !out || !size)
		return 1;
	if (max_size < 6u + PINNED_TIFF_HEADER_SIZE ||
	    max_size > PINNED_MAX_SIZE)
		return 1;

	memset (&l, 0, sizeof (l));
	l.data = data;
	l.mem = exif_data_get_mem (data);
	l.order = exif_data_get_byte_order (data);
	if (!l.mem)
		return 1;
	l.note_block = ~0u;

	rc = pinned_build (&l, max_size - 6u);
	if (rc == 0)
		rc = pinned_pack (&l, max_size - 6u);
	if (rc == 0)
		rc = pinned_emit (&l);

	if (rc != 0) {
		pinned_layout_free (&l);
		return 1;
	}

	/* Defensive final validation against the caller bound. */
	if (6u + l.size > max_size || 6u + l.size > PINNED_MAX_SIZE) {
		pinned_layout_free (&l);
		return 1;
	}

	*out = l.out;
	*size = 6u + l.size;
	l.out = NULL;	/* ownership transferred to the caller */
	pinned_layout_free (&l);
	return 0;
}
