/* pinned_contract.c
 *
 * Contract tests for exif_data_save_data_pinned(). All fixtures are
 * generated in C and every output is re-parsed by an independent reader that
 * never calls into libexif's writing machinery.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "contract.h"

#include <libexif/exif-data.h>
#include <libexif/exif-content.h>
#include <libexif/exif-entry.h>
#include <libexif/exif-format.h>
#include <libexif/exif-ifd.h>
#include <libexif/exif-mem.h>
#include <libexif/exif-tag.h>
#include <libexif/exif-utils.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================ */
/* Byte order helpers                                           */
/* ============================================================ */

static ExifShort
g16 (const unsigned char *p, ExifByteOrder o)
{
	return exif_get_short (p, o);
}

static ExifLong
g32 (const unsigned char *p, ExifByteOrder o)
{
	return exif_get_long (p, o);
}

static void
s16 (unsigned char *p, ExifByteOrder o, ExifShort v)
{
	exif_set_short (p, o, v);
}

static void
s32 (unsigned char *p, ExifByteOrder o, ExifLong v)
{
	exif_set_long (p, o, v);
}

static unsigned int
even_up (unsigned int n)
{
	return (n + 1u) & ~1u;
}

/* ============================================================ */
/* Independent raw TIFF builder                                 */
/* ============================================================ */

struct r_entry {
	ExifTag tag;
	ExifShort format;
	ExifLong components;
	const unsigned char *value;
	unsigned int size;	/* raw byte size */
	unsigned int valblk;	/* assigned value offset, 0 if inline */
};

struct r_ifd {
	struct r_entry *entries;
	unsigned int count;
};

#define R_IFD_0 0
#define R_IFD_EXIF 1
#define R_IFD_GPS 2
#define R_IFD_INTEROP 3
#define R_IFD_1 4
#define R_NIFD 5

struct build_spec {
	ExifByteOrder order;
	struct r_ifd ifd[R_NIFD];
	unsigned int note_offset;	/* forced TIFF offset, 0 = automatic */
	const unsigned char *note_bytes;
	unsigned int note_size;
	const unsigned char *thumb;
	unsigned int thumb_size;
};

struct raw_doc {
	unsigned char *buf;
	unsigned int size;
	unsigned int note_offset;
	unsigned int note_size;
	ExifByteOrder order;
};

struct slot {
	unsigned int start;
	unsigned int end;
};

/* Earliest-fit allocator that keeps every placed range disjoint and every
 * start even. A forced MakerNote is reserved first, so other blocks land in
 * whichever free region (front or back) fits. */
struct placer {
	struct slot *slots;
	unsigned int nslots;
	unsigned int capacity;
};

static int
placer_reserve (struct placer *p, unsigned int start, unsigned int size)
{
	struct slot *ns = realloc (p->slots,
				   (p->nslots + 1u) * sizeof (*ns));
	unsigned int end = start + size;
	if (!ns)
		return -1;
	p->slots = ns;
	p->slots[p->nslots].start = start;
	p->slots[p->nslots].end = end;
	p->nslots++;
	if (end > p->capacity)
		p->capacity = end;
	return 0;
}

static int
placer_place (struct placer *p, unsigned int size, unsigned int *out)
{
	unsigned int cand = 8;
	unsigned int i;

	size = even_up (size);
	for (;;) {
		int conflict = 0;
		for (i = 0; i < p->nslots; i++) {
			if (cand < p->slots[i].end &&
			    p->slots[i].start < cand + size) {
				cand = even_up (p->slots[i].end);
				conflict = 1;
				break;
			}
		}
		if (!conflict)
			break;
	}
	if (placer_reserve (p, cand, size) < 0)
		return -1;
	*out = cand;
	return 0;
}

static int
raw_build (struct build_spec *sp, struct raw_doc *out)
{
	unsigned int i, k;
	struct placer pl;
	unsigned int dir_off[R_NIFD];
	unsigned int dir_entries[R_NIFD];
	unsigned int thumb_off = 0;
	unsigned char *buf;

	memset (&pl, 0, sizeof (pl));
	memset (dir_off, 0, sizeof (dir_off));
	memset (dir_entries, 0, sizeof (dir_entries));
	memset (out, 0, sizeof (*out));

	if (sp->note_size && sp->note_offset) {
		if (sp->note_offset < 8u)
			return -1;
		if (placer_reserve (&pl, sp->note_offset,
				    even_up (sp->note_size)) < 0)
			return -1;
	}

	/* Directory geometry. */
	for (i = 0; i < R_NIFD; i++) {
		unsigned int entries = sp->ifd[i].count;
		if (i == R_IFD_0) {
			if (sp->ifd[R_IFD_EXIF].count)
				entries++;
			if (sp->ifd[R_IFD_GPS].count)
				entries++;
		}
		if (i == R_IFD_EXIF && sp->ifd[R_IFD_INTEROP].count)
			entries++;
		if (i == R_IFD_1 && sp->thumb_size)
			entries += 2;
		dir_entries[i] = entries;
		if (entries) {
			if (placer_place (&pl, 2u + 12u * entries + 4u,
					  &dir_off[i]) < 0)
				goto fail;
		}
	}

	/* External value blocks. */
	for (i = 0; i < R_NIFD; i++) {
		for (k = 0; k < sp->ifd[i].count; k++) {
			struct r_entry *e = &sp->ifd[i].entries[k];
			e->valblk = 0;
			if (e->tag == EXIF_TAG_MAKER_NOTE || e->size <= 4)
				continue;
			if (placer_place (&pl, e->size, &e->valblk) < 0)
				goto fail;
		}
	}

	/* Auto-positioned MakerNote goes after everything else. */
	if (sp->note_size && !sp->note_offset) {
		if (placer_place (&pl, sp->note_size,
				  &sp->note_offset) < 0)
			goto fail;
	}

	if (sp->thumb_size) {
		if (placer_place (&pl, sp->thumb_size, &thumb_off) < 0)
			goto fail;
	}

	buf = calloc (1, pl.capacity ? pl.capacity : 8);
	if (!buf)
		goto fail;

	memcpy (buf, sp->order == EXIF_BYTE_ORDER_INTEL ? "II" : "MM", 2);
	s16 (buf + 2, sp->order, 0x002a);
	s32 (buf + 4, sp->order, dir_off[R_IFD_0]);

	if (sp->note_size)
		memcpy (buf + sp->note_offset, sp->note_bytes, sp->note_size);
	if (sp->thumb_size)
		memcpy (buf + thumb_off, sp->thumb, sp->thumb_size);

	/* Directory contents. */
	for (i = 0; i < R_NIFD; i++) {
		struct r_ifd *ri = &sp->ifd[i];
		unsigned int base = dir_off[i];
		unsigned int slot = 0;

		if (!dir_entries[i])
			continue;
		s16 (buf + base, sp->order,
		     (ExifShort) dir_entries[i]);

		for (k = 0; k < ri->count; k++) {
			struct r_entry *e = &ri->entries[k];
			unsigned int pos = base + 2u + 12u * slot++;
			unsigned int voff;
			s16 (buf + pos + 0, sp->order, (ExifShort) e->tag);
			s16 (buf + pos + 2, sp->order, e->format);
			s32 (buf + pos + 4, sp->order, e->components);
			if (e->tag == EXIF_TAG_MAKER_NOTE)
				voff = sp->note_offset;
			else if (e->size > 4)
				voff = e->valblk;
			else
				voff = 0;
			if (e->size > 4) {
				s32 (buf + pos + 8, sp->order, voff);
				memcpy (buf + voff, e->value, e->size);
			} else if (e->value) {
				memcpy (buf + pos + 8, e->value, e->size);
			}
		}

		if (i == R_IFD_0) {
			if (sp->ifd[R_IFD_EXIF].count) {
				unsigned int pos = base + 2u + 12u * slot++;
				s16 (buf + pos, sp->order,
				     EXIF_TAG_EXIF_IFD_POINTER);
				s16 (buf + pos + 2, sp->order,
				     EXIF_FORMAT_LONG);
				s32 (buf + pos + 4, sp->order, 1);
				s32 (buf + pos + 8, sp->order,
				     dir_off[R_IFD_EXIF]);
			}
			if (sp->ifd[R_IFD_GPS].count) {
				unsigned int pos = base + 2u + 12u * slot++;
				s16 (buf + pos, sp->order,
				     EXIF_TAG_GPS_INFO_IFD_POINTER);
				s16 (buf + pos + 2, sp->order,
				     EXIF_FORMAT_LONG);
				s32 (buf + pos + 4, sp->order, 1);
				s32 (buf + pos + 8, sp->order,
				     dir_off[R_IFD_GPS]);
			}
		}
		if (i == R_IFD_EXIF && sp->ifd[R_IFD_INTEROP].count) {
			unsigned int pos = base + 2u + 12u * slot++;
			s16 (buf + pos, sp->order,
			     EXIF_TAG_INTEROPERABILITY_IFD_POINTER);
			s16 (buf + pos + 2, sp->order, EXIF_FORMAT_LONG);
			s32 (buf + pos + 4, sp->order, 1);
			s32 (buf + pos + 8, sp->order, dir_off[R_IFD_INTEROP]);
		}
		if (i == R_IFD_1 && sp->thumb_size) {
			unsigned int p1 = base + 2u + 12u * slot++;
			unsigned int p2 = base + 2u + 12u * slot++;
			s16 (buf + p1, sp->order,
			     EXIF_TAG_JPEG_INTERCHANGE_FORMAT);
			s16 (buf + p1 + 2, sp->order, EXIF_FORMAT_LONG);
			s32 (buf + p1 + 4, sp->order, 1);
			s32 (buf + p1 + 8, sp->order, thumb_off);
			s16 (buf + p2, sp->order,
			     EXIF_TAG_JPEG_INTERCHANGE_FORMAT_LENGTH);
			s16 (buf + p2 + 2, sp->order, EXIF_FORMAT_LONG);
			s32 (buf + p2 + 4, sp->order, 1);
			s32 (buf + p2 + 8, sp->order, sp->thumb_size);
		}

		if (i == R_IFD_0 && dir_entries[R_IFD_1])
			s32 (buf + base + 2u + 12u * dir_entries[i],
			     sp->order, dir_off[R_IFD_1]);
		else
			s32 (buf + base + 2u + 12u * dir_entries[i],
			     sp->order, 0);
	}

	free (pl.slots);
	out->buf = buf;
	out->size = pl.capacity;
	out->note_offset = sp->note_offset;
	out->note_size = sp->note_size;
	out->order = sp->order;
	return 0;

fail:
	free (pl.slots);
	return -1;
}

/* ============================================================ */
/* Independent TIFF reader / structural verifier                */
/* ============================================================ */

struct v_range {
	unsigned int start;
	unsigned int end;
};

struct v_ifd {
	unsigned int offset;
	unsigned int count;
	int present;
};

struct verdict {
	ExifByteOrder order;
	struct v_ifd ifd[R_NIFD];
	struct v_range *ranges;
	unsigned int nranges;
	int note_found;
	unsigned int note_off, note_size;
	unsigned int internal_target;
	int internal_inside;
	int thumb_found;
	unsigned int thumb_off, thumb_size;
};

static int
range_add (struct verdict *v, unsigned int start, unsigned int size)
{
	struct v_range *nr;
	unsigned int i;
	unsigned int end = start + size;

	if (start & 1u)
		return -1;
	for (i = 0; i < v->nranges; i++) {
		if (start < v->ranges[i].end && v->ranges[i].start < end)
			return -1;
	}
	nr = realloc (v->ranges, (v->nranges + 1u) * sizeof (*nr));
	if (!nr)
		return -1;
	v->ranges = nr;
	v->ranges[v->nranges].start = start;
	v->ranges[v->nranges].end = end;
	v->nranges++;
	return 0;
}

static int
walk_ifd (const unsigned char *buf, unsigned int size,
	  ExifByteOrder o, unsigned int off, struct verdict *v,
	  int which, unsigned char *seen)
{
	unsigned int n, i;
	ExifTag prev = 0;

	if (off == 0)
		return 0;
	if (off + 6u > size || seen[which])
		return -1;
	seen[which] = 1;
	n = g16 (buf + off, o);
	if (off + 2u + 12u * n + 4u > size)
		return -1;

	v->ifd[which].present = 1;
	v->ifd[which].offset = off;
	v->ifd[which].count = n;
	if (range_add (v, off, 2u + 12u * n + 4u) < 0)
		return -1;

	for (i = 0; i < n; i++) {
		unsigned int pos = off + 2u + 12u * i;
		ExifTag tag = g16 (buf + pos, o);
		ExifShort fmt = g16 (buf + pos + 2, o);
		ExifLong comp = g32 (buf + pos + 4, o);
		ExifLong val = g32 (buf + pos + 8, o);
		unsigned char fs;
		unsigned int bytes;

		if (i && tag <= prev)
			return -1;	/* directories must be tag sorted */
		prev = tag;

		fs = exif_format_get_size (fmt);
		if (!fs || comp == 0 || comp > 0xffffffffUL / fs)
			return -1;
		bytes = (unsigned int) (comp * (unsigned long) fs);

		if (tag == EXIF_TAG_EXIF_IFD_POINTER) {
			if (walk_ifd (buf, size, o, val, v,
				      R_IFD_EXIF, seen) < 0)
				return -1;
			continue;
		}
		if (tag == EXIF_TAG_GPS_INFO_IFD_POINTER) {
			if (walk_ifd (buf, size, o, val, v,
				      R_IFD_GPS, seen) < 0)
				return -1;
			continue;
		}
		if (tag == EXIF_TAG_INTEROPERABILITY_IFD_POINTER) {
			if (walk_ifd (buf, size, o, val, v,
				      R_IFD_INTEROP, seen) < 0)
				return -1;
			continue;
		}
		if (tag == EXIF_TAG_JPEG_INTERCHANGE_FORMAT) {
			v->thumb_found = 1;
			v->thumb_off = val;
			continue;
		}
		if (tag == EXIF_TAG_JPEG_INTERCHANGE_FORMAT_LENGTH) {
			v->thumb_size = val;
			continue;
		}
		if (tag == EXIF_TAG_MAKER_NOTE && bytes > 4) {
			v->note_found = 1;
			v->note_off = val;
			v->note_size = bytes;
			if (range_add (v, val, even_up (bytes)) < 0)
				return -1;
			continue;
		}
		if (bytes > 4) {
			if (val + bytes > size)
				return -1;
			if (range_add (v, val, even_up (bytes)) < 0)
				return -1;
		}
	}

	if (which == R_IFD_0) {
		ExifLong next = g32 (buf + off + 2u + 12u * n, o);
		if (next && walk_ifd (buf, size, o, next, v,
				      R_IFD_1, seen) < 0)
			return -1;
	} else if (g32 (buf + off + 2u + 12u * n, o) != 0) {
		return -1;
	}
	return 0;
}

static int
verify_tiff (const unsigned char *raw, unsigned int size,
	     struct verdict *v)
{
	ExifByteOrder o;
	unsigned char seen[R_NIFD];

	memset (v, 0, sizeof (*v));
	memset (seen, 0, sizeof (seen));
	if (size < 8)
		return -1;
	if (!memcmp (raw, "II", 2))
		o = EXIF_BYTE_ORDER_INTEL;
	else if (!memcmp (raw, "MM", 2))
		o = EXIF_BYTE_ORDER_MOTOROLA;
	else
		return -1;
	v->order = o;
	if (g16 (raw + 2, o) != 0x002a)
		return -1;
	if (walk_ifd (raw, size, o, g32 (raw + 4, o), v,
		      R_IFD_0, seen) < 0)
		return -1;
	if (v->thumb_found) {
		if (v->thumb_off + v->thumb_size > size)
			return -1;
		if (range_add (v, v->thumb_off,
			       even_up (v->thumb_size)) < 0)
			return -1;
	}
	if (v->note_found) {
		/* Every fixture stores its internal TIFF-relative long at
		 * note+8, pointing at a marker inside the block. */
		if (v->note_size < 12)
			return -1;
		v->internal_target = g32 (raw + v->note_off + 8, o);
		v->internal_inside =
			(v->internal_target >= v->note_off &&
			 v->internal_target + 4u <=
			 v->note_off + v->note_size);
	}
	free (v->ranges);
	v->ranges = NULL;
	v->nranges = 0;
	return 0;
}

/* ============================================================ */
/* Packing reference: exhaustive partition vs. dumb greedy      */
/* ============================================================ */

struct pack_problem {
	const unsigned int *sizes;
	unsigned int n;
	unsigned int note_start;
	unsigned int note_end;
	unsigned int capacity;
};

static void
pack_rooms (const struct pack_problem *p,
	    unsigned int *front_room, unsigned int *back_room)
{
	*front_room = p->note_start ? p->note_start - 8u :
				     p->capacity - 8u;
	*back_room = p->note_start ? p->capacity - p->note_end : 0u;
}

static int
oracle_exhaustive (const struct pack_problem *p)
{
	unsigned long combos = 1UL << p->n;
	unsigned long mask;
	unsigned int front_room, back_room, i;

	pack_rooms (p, &front_room, &back_room);
	for (mask = 0; mask < combos; mask++) {
		unsigned int front = 0, back = 0;
		for (i = 0; i < p->n; i++) {
			if (mask & (1UL << i))
				back += p->sizes[i];
			else
				front += p->sizes[i];
		}
		if (front <= front_room && back <= back_room)
			return 1;
	}
	return 0;
}

/* Deliberately dumb first-fit: front while it fits, otherwise back. */
static int
oracle_greedy (const struct pack_problem *p)
{
	unsigned int front = 0, back = 0, i;
	unsigned int front_room, back_room;

	pack_rooms (p, &front_room, &back_room);
	for (i = 0; i < p->n; i++) {
		if (front + p->sizes[i] <= front_room)
			front += p->sizes[i];
		else if (back + p->sizes[i] <= back_room)
			back += p->sizes[i];
		else
			return 0;
	}
	return 1;
}

/* Exact subset-sum oracle for block sets too large to enumerate. */
static int
oracle_subset_sum (const struct pack_problem *p)
{
	unsigned int front_room, back_room, total = 0, target_min, s, i;
	unsigned char *reach;

	pack_rooms (p, &front_room, &back_room);
	for (i = 0; i < p->n; i++)
		total += p->sizes[i];
	if (total > front_room + back_room)
		return 0;
	target_min = (total > back_room) ? (total - back_room) : 0;
	if (target_min > front_room)
		return 0;
	reach = calloc (front_room + 1u, 1);
	if (!reach)
		return -1;
	reach[0] = 1;
	for (i = 0; i < p->n; i++) {
		unsigned int w = p->sizes[i];
		for (s = front_room + 1u; s-- > w; ) {
			if (reach[s - w])
				reach[s] = 1;
		}
	}
	for (s = target_min; s <= front_room; s++) {
		if (reach[s]) {
			free (reach);
			return 1;
		}
	}
	free (reach);
	return 0;
}

/* ============================================================ */
/* ExifData helpers                                             */
/* ============================================================ */

static ExifData *
load_doc (ExifMem *mem, const unsigned char *tiff, unsigned int size)
{
	ExifData *d;
	unsigned char *wrapped;

	/* The loader expects the six byte "Exif\0\0" marker before the
	 * TIFF block; offsets inside stay relative to the TIFF header. */
	wrapped = malloc (6u + size);
	if (!wrapped)
		return NULL;
	memcpy (wrapped, "Exif\0\0", 6);
	memcpy (wrapped + 6, tiff, size);

	d = exif_data_new_mem (mem);
	if (!d) {
		free (wrapped);
		return NULL;
	}
	exif_data_unset_option (d, EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
	exif_data_unset_option (d, EXIF_DATA_OPTION_IGNORE_UNKNOWN_TAGS);
	exif_data_set_option (d, EXIF_DATA_OPTION_DONT_CHANGE_MAKER_NOTE);
	exif_data_load_data (d, wrapped, 6u + size);
	free (wrapped);
	return d;
}

static ExifEntry *
add_undefined (ExifMem *mem, ExifData *d, ExifIfd ifd, ExifTag tag,
	       const void *bytes, unsigned int len)
{
	ExifEntry *e = exif_entry_new_mem (mem);
	unsigned char *buf;
	if (!e)
		return NULL;
	e->tag = tag;
	e->format = EXIF_FORMAT_UNDEFINED;
	e->components = len;
	buf = exif_mem_alloc (mem, len ? len : 1);
	if (!buf) {
		exif_entry_unref (e);
		return NULL;
	}
	memcpy (buf, bytes, len);
	e->data = buf;
	e->size = len;
	exif_content_add_entry (d->ifd[ifd], e);
	exif_entry_unref (e);
	return e;
}

static ExifEntry *
add_ascii (ExifMem *mem, ExifData *d, ExifIfd ifd, ExifTag tag,
	   const char *text)
{
	return add_undefined (mem, d, ifd, tag, text,
			      (unsigned int) strlen (text) + 1u);
}

static ExifEntry *
add_short_value (ExifMem *mem, ExifData *d, ExifIfd ifd, ExifTag tag,
		 ExifShort value)
{
	ExifEntry *e = exif_entry_new_mem (mem);
	if (!e)
		return NULL;
	e->tag = tag;
	e->format = EXIF_FORMAT_SHORT;
	e->components = 1;
	e->data = exif_mem_alloc (mem, 2);
	if (!e->data) {
		exif_entry_unref (e);
		return NULL;
	}
	e->size = 2;
	exif_set_short (e->data, exif_data_get_byte_order (d), value);
	exif_content_add_entry (d->ifd[ifd], e);
	exif_entry_unref (e);
	return e;
}

/* Standard fixture MakerNote: magic, pad, an internal TIFF-relative long at
 * note+8 pointing at the marker at note+12, then the marker. */
static void
make_note (unsigned char note[16], ExifByteOrder o, unsigned int note_off)
{
	memset (note, 0, 16);
	memcpy (note, "UFN0", 4);
	s32 (note + 8, o, note_off + 12);
	memcpy (note + 12, "MARK", 4);
}

static void
note_fixture (struct r_entry *e, unsigned char *note, unsigned int size)
{
	e->tag = EXIF_TAG_MAKER_NOTE;
	e->format = EXIF_FORMAT_UNDEFINED;
	e->components = size;
	e->value = note;
	e->size = size;
	e->valblk = 0;
}

/* ============================================================ */
/* addresses group                                              */
/* ============================================================ */

static int
case_pinned_keeps_position (ExifByteOrder o)
{
	const unsigned int note_off = 128;
	unsigned char note[16];
	unsigned char desc[40];
	struct r_entry ifd0[1];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	ExifEntry *e;
	unsigned char *out = NULL;
	unsigned int size = 0;
	struct verdict v;
	int rc;

	make_note (note, o, note_off);
	memset (desc, 'D', sizeof (desc));
	desc[sizeof (desc) - 1] = 0;

	ifd0[0].tag = EXIF_TAG_IMAGE_DESCRIPTION;
	ifd0[0].format = EXIF_FORMAT_ASCII;
	ifd0[0].components = sizeof (desc);
	ifd0[0].value = desc;
	ifd0[0].size = sizeof (desc);
	ifd0[0].valblk = 0;
	note_fixture (&exif[0], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_0].entries = ifd0;
	sp.ifd[R_IFD_0].count = 1;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;

	e = exif_content_get_entry (d->ifd[EXIF_IFD_0],
				    EXIF_TAG_IMAGE_DESCRIPTION);
	if (!e)
		return 3;
	memset (e->data, 'Z', e->size - 1);
	if (!add_short_value (mem, d, EXIF_IFD_0,
			      EXIF_TAG_ORIENTATION, 6))
		return 4;

	rc = exif_data_save_data_pinned (d, 65533, &out, &size);
	if (rc || !out || !size || size > 65533)
		return 5;

	if (verify_tiff (out + 6, size - 6, &v) < 0)
		return 6;
	if (!v.note_found || v.note_off != note_off)
		return 7;
	if (memcmp (out + 6 + note_off, note, 16) != 0)
		return 8;
	if (!v.internal_inside ||
	    v.internal_target != note_off + 12 ||
	    memcmp (out + 6 + v.internal_target, "MARK", 4) != 0)
		return 9;
	if (!v.ifd[R_IFD_EXIF].present || !v.ifd[R_IFD_0].present)
		return 10;

	{
		ExifData *r = exif_data_new_from_data (out, size);
		ExifEntry *mn, *ori;
		if (!r)
			return 11;
		mn = exif_content_get_entry (r->ifd[EXIF_IFD_EXIF],
					     EXIF_TAG_MAKER_NOTE);
		if (!mn || mn->size != 16 ||
		    memcmp (mn->data, note, 16) != 0)
			return 12;
		ori = exif_content_get_entry (r->ifd[EXIF_IFD_0],
					      EXIF_TAG_ORIENTATION);
		if (!ori || exif_get_short (ori->data, o) != 6)
			return 13;
		exif_data_unref (r);
	}

	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_addresses_ii (void)
{
	return case_pinned_keeps_position (EXIF_BYTE_ORDER_INTEL);
}

static int
case_addresses_mm (void)
{
	return case_pinned_keeps_position (EXIF_BYTE_ORDER_MOTOROLA);
}

static int
case_note_near_tail (void)
{
	/* MakerNote close to the segment tail: added data only fits in the
	 * free region in front of it. */
	const unsigned int note_off = 200;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	unsigned char big[96];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned char *out = NULL;
	unsigned int size = 0;
	struct verdict v;

	make_note (note, o, note_off);
	memset (big, 'B', sizeof (big));
	big[sizeof (big) - 1] = 0;
	note_fixture (&exif[0], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;
	if (!add_ascii (mem, d, EXIF_IFD_0, EXIF_TAG_IMAGE_DESCRIPTION,
			(const char *) big))
		return 3;

	if (exif_data_save_data_pinned (d, 260, &out, &size) != 0)
		return 4;
	if (verify_tiff (out + 6, size - 6, &v) < 0)
		return 5;
	if (v.note_off != note_off)
		return 6;
	{
		ExifData *r = exif_data_new_from_data (out, size);
		ExifEntry *de;
		if (!r)
			return 7;
		de = exif_content_get_entry (r->ifd[EXIF_IFD_0],
					     EXIF_TAG_IMAGE_DESCRIPTION);
		if (!de || de->size != sizeof (big) ||
		    memcmp (de->data, big, sizeof (big)) != 0)
			return 8;
		exif_data_unref (r);
	}

	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_reject_byte_order_switch (void)
{
	const unsigned int note_off = 128;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned char *out = (unsigned char *) 1;
	unsigned int size = 1;

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;
	exif_data_set_byte_order (d, EXIF_BYTE_ORDER_MOTOROLA);
	if (exif_data_save_data_pinned (d, 65533, &out, &size) == 0)
		return 3;
	if (out != NULL || size != 0)
		return 4;

	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_reject_tamper_and_recover (void)
{
	const unsigned int note_off = 128;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d, *r;
	ExifEntry *mn;
	unsigned char *out = NULL;
	unsigned int size = 0;

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;
	mn = exif_content_get_entry (d->ifd[EXIF_IFD_EXIF],
				     EXIF_TAG_MAKER_NOTE);
	if (!mn)
		return 3;

	mn->data[0] ^= 0xff;
	if (exif_data_save_data_pinned (d, 65533, &out, &size) == 0)
		return 4;
	if (out != NULL || size != 0)
		return 5;

	/* Restore the original bytes: the pin proves itself again. */
	mn->data[0] = note[0];
	if (exif_data_save_data_pinned (d, 65533, &out, &size) != 0)
		return 6;
	r = exif_data_new_from_data (out, size);
	if (!r ||
	    !exif_content_get_entry (r->ifd[EXIF_IFD_EXIF],
				     EXIF_TAG_MAKER_NOTE))
		return 7;
	exif_data_unref (r);
	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_reject_remove_recreate (void)
{
	const unsigned int note_off = 128;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	ExifEntry *mn;
	unsigned char *out = (unsigned char *) 1;
	unsigned int size = 1;

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;
	mn = exif_content_get_entry (d->ifd[EXIF_IFD_EXIF],
				     EXIF_TAG_MAKER_NOTE);
	if (!mn)
		return 3;
	exif_content_remove_entry (d->ifd[EXIF_IFD_EXIF], mn);
	if (!add_undefined (mem, d, EXIF_IFD_EXIF, EXIF_TAG_MAKER_NOTE,
			    note, 16))
		return 4;

	if (exif_data_save_data_pinned (d, 65533, &out, &size) == 0)
		return 5;
	if (out != NULL || size != 0)
		return 6;

	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_default_options_pin (void)
{
	/* A plain exif_data_load_data() with the default option set must
	 * still permit pinning: exif_data_fix never rewrites a MakerNote. */
	const unsigned int note_off = 128;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned char *out = NULL;
	unsigned int size = 0;
	unsigned char *wrapped;
	struct verdict v;

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);
	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	wrapped = malloc (6u + doc.size);
	if (!wrapped)
		return 2;
	memcpy (wrapped, "Exif\0\0", 6);
	memcpy (wrapped + 6, doc.buf, doc.size);

	mem = exif_mem_new_default ();
	d = exif_data_new_mem (mem);
	if (!d)
		return 3;
	exif_data_load_data (d, wrapped, 6u + doc.size);
	free (wrapped);
	free (doc.buf);

	if (exif_data_save_data_pinned (d, 65533, &out, &size) != 0)
		return 4;
	if (verify_tiff (out + 6, size - 6, &v) < 0 ||
	    v.note_off != note_off ||
	    memcmp (out + 6 + note_off, note, 16) != 0)
		return 5;

	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	return 0;
}

static int
case_unknown_signature_accepted (void)
{
	/* No vendor support list: an arbitrary signature must be pinned. */
	const unsigned int note_off = 160;
	const unsigned int note_size = 20;
	ExifByteOrder o = EXIF_BYTE_ORDER_MOTOROLA;
	unsigned char note[20];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned char *out = NULL;
	unsigned int size = 0;
	struct verdict v;

	memset (note, 0, sizeof (note));
	memcpy (note, "ZZ-VENDOR-9", 11);
	s32 (note + 8, o, note_off + 16);
	memcpy (note + 16, "MARK", 4);
	note_fixture (&exif[0], note, note_size);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = note_size;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;
	if (exif_data_save_data_pinned (d, 65533, &out, &size) != 0)
		return 3;
	if (verify_tiff (out + 6, size - 6, &v) < 0)
		return 4;
	if (v.note_off != note_off ||
	    memcmp (out + 6 + note_off, note, note_size) != 0 ||
	    !v.internal_inside ||
	    v.internal_target != note_off + 16)
		return 5;

	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

/* ============================================================ */
/* packing group                                                */
/* ============================================================ */

/* Mirror the writer's movable block enumeration (directories, external
 * values, thumbnail). The pinned note is accounted for by note_start/end. */
static int
collect_blocks (ExifData *d, unsigned int **sizes_out, unsigned int *n_out)
{
	unsigned int cap = 16, n = 0, i, j;
	unsigned int *sizes = calloc (cap, sizeof (unsigned int));
	if (!sizes)
		return -1;

	for (i = 0; i < EXIF_IFD_COUNT; i++) {
		ExifContent *c = d->ifd[i];
		unsigned int extra = 0, entries;
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
		entries = c->count + extra;
		if (i == EXIF_IFD_0 || entries) {
			if (n == cap) {
				unsigned int *t = realloc (sizes,
					(cap * 2u) * sizeof (unsigned int));
				if (!t) {
					free (sizes);
					return -1;
				}
				sizes = t;
				cap *= 2;
			}
			sizes[n++] = even_up (2u + 12u * entries + 4u);
		}
		for (j = 0; j < c->count; j++) {
			ExifEntry *e = c->entries[j];
			unsigned char fs = exif_format_get_size (e->format);
			unsigned int bytes;
			if (!fs || e->components == 0) {
				free (sizes);
				return -1;
			}
			bytes = (unsigned int)
				(e->components * (unsigned long) fs);
			if (bytes > 4 && e->tag != EXIF_TAG_MAKER_NOTE) {
				if (n == cap) {
					unsigned int *t = realloc (sizes,
						(cap * 2u) *
						sizeof (unsigned int));
					if (!t) {
						free (sizes);
						return -1;
					}
					sizes = t;
					cap *= 2;
				}
				sizes[n++] = even_up (bytes);
			}
		}
	}
	if (d->size) {
		unsigned int *t = realloc (sizes,
			(n + 1u) * sizeof (unsigned int));
		if (!t) {
			free (sizes);
			return -1;
		}
		sizes = t;
		sizes[n++] = even_up (d->size);
	}
	*sizes_out = sizes;
	*n_out = n;
	return 0;
}

static int
case_oracle_matches (void)
{
	/* Real documents over many capacities: the library's verdict must
	 * match the exhaustive partition, and it must never report a space
	 * shortage where first-fit greedy would give up. */
	static const unsigned char lens[] = {5, 6, 7, 8, 9, 10, 12, 16};
	static const unsigned char filler[] = "0123456789abcdef";
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	const unsigned int note_off = 96;
	unsigned char note[16];
	unsigned int trial;

	make_note (note, o, note_off);

	for (trial = 0; trial < 200; trial++) {
		struct r_entry exif[1];
		struct r_entry vals[8];
		struct build_spec sp;
		struct raw_doc doc;
		ExifMem *mem;
		ExifData *d;
		unsigned char *out = NULL;
		unsigned int size = 0;
		unsigned int *sizes = NULL, nsizes = 0;
		unsigned int capacity = 180u + (trial % 90u);
		struct pack_problem pp;
		int expected, got;
		unsigned int k, nval;

		memset (vals, 0, sizeof (vals));
		nval = 1u + (trial % 6u);
		for (k = 0; k < nval; k++) {
			unsigned int len = lens[(trial + k) %
				(sizeof (lens) / sizeof (lens[0]))];
			vals[k].tag = (ExifTag) (0xc000 + k);
			vals[k].format = EXIF_FORMAT_UNDEFINED;
			vals[k].components = len;
			vals[k].value = filler;
			vals[k].size = len;
		}
		note_fixture (&exif[0], note, 16);

		memset (&sp, 0, sizeof (sp));
		sp.order = o;
		sp.ifd[R_IFD_EXIF].entries = exif;
		sp.ifd[R_IFD_EXIF].count = 1;
		sp.ifd[R_IFD_0].entries = vals;
		sp.ifd[R_IFD_0].count = nval;
		sp.note_offset = note_off;
		sp.note_bytes = note;
		sp.note_size = 16;
		if (raw_build (&sp, &doc) < 0)
			return 1;

		mem = exif_mem_new_default ();
		d = load_doc (mem, doc.buf, doc.size);
		if (!d)
			return 2;
		if (collect_blocks (d, &sizes, &nsizes) < 0)
			return 3;

		pp.sizes = sizes;
		pp.n = nsizes;
		pp.note_start = note_off;
		pp.note_end = even_up (note_off + 16);
		pp.capacity = capacity;
		expected = oracle_exhaustive (&pp);
		got = (exif_data_save_data_pinned (d, 6u + capacity,
						    &out, &size) == 0);
		if (got != expected) {
			fprintf (stderr,
				 "oracle mismatch trial=%u expected=%d got=%d "
				 "cap=%u blocks=%u\n",
				 trial, expected, got, capacity, nsizes);
			return 4;
		}
		if (got) {
			struct verdict v;
			if (size > 6u + capacity)
				return 5;
			if (verify_tiff (out + 6, size - 6, &v) < 0)
				return 6;
			if (v.note_off != note_off)
				return 7;
			exif_mem_free (mem, out);
		} else if (out != NULL || size != 0) {
			return 8;
		}

		free (sizes);
		exif_data_unref (d);
		exif_mem_unref (mem);
		free (doc.buf);
	}
	return 0;
}

static int
case_greedy_vs_exhaustive (void)
{
	/* Abstract sanity check on the two reference oracles: total space
	 * suffices (rooms 12 + 8), but first-fit in input order fails while
	 * a partition exists for blocks 8, 6, 6. */
	unsigned int sizes[] = {8, 6, 6};
	struct pack_problem pp;

	pp.sizes = sizes;
	pp.n = 3;
	pp.note_start = 20;	/* front room 12 */
	pp.note_end = 20;
	pp.capacity = 28;	/* back room 8 */
	if (oracle_greedy (&pp))
		return 1;	/* precondition: greedy really does fail */
	if (!oracle_exhaustive (&pp))
		return 2;	/* precondition: a partition exists */

	/* And a genuinely impossible instance with total space short. */
	{
		unsigned int bad[] = {8, 8, 8};
		struct pack_problem q;
		q.sizes = bad;
		q.n = 3;
		q.note_start = 20;
		q.note_end = 20;
		q.capacity = 28;	/* rooms 12 + 8 = 20 < 24 */
		if (oracle_exhaustive (&q))
			return 3;
	}

	/* Same pitfall on a real loaded document: search for a capacity at
	 * which the greedy reference fails but an exact partition exists,
	 * then require the library to find that layout. */
	{
		const unsigned int note_off = 96;
		ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
		unsigned char note[16];
		static const unsigned char b8[8] = "8888888";
		static const unsigned char b6a[6] = "66666";
		static const unsigned char b6b[6] = "77777";
		struct r_entry exif[1];
		struct r_entry vals[3];
		struct build_spec sp;
		struct raw_doc doc;
		ExifMem *mem;
		ExifData *d;
		unsigned char *out = NULL;
		unsigned int size = 0;
		unsigned int *blocks = NULL, nblocks = 0, cap;
		struct verdict v;
		int found = 0;

		make_note (note, o, note_off);
		vals[0].tag = (ExifTag) 0xc300;
		vals[0].format = EXIF_FORMAT_UNDEFINED;
		vals[0].components = sizeof (b8);
		vals[0].value = b8;
		vals[0].size = sizeof (b8);
		vals[0].valblk = 0;
		vals[1].tag = (ExifTag) 0xc301;
		vals[1].format = EXIF_FORMAT_UNDEFINED;
		vals[1].components = sizeof (b6a);
		vals[1].value = b6a;
		vals[1].size = sizeof (b6a);
		vals[1].valblk = 0;
		vals[2].tag = (ExifTag) 0xc302;
		vals[2].format = EXIF_FORMAT_UNDEFINED;
		vals[2].components = sizeof (b6b);
		vals[2].value = b6b;
		vals[2].size = sizeof (b6b);
		vals[2].valblk = 0;
		note_fixture (&exif[0], note, 16);

		memset (&sp, 0, sizeof (sp));
		sp.order = o;
		sp.ifd[R_IFD_0].entries = vals;
		sp.ifd[R_IFD_0].count = 3;
		sp.ifd[R_IFD_EXIF].entries = exif;
		sp.ifd[R_IFD_EXIF].count = 1;
		sp.note_offset = note_off;
		sp.note_bytes = note;
		sp.note_size = 16;
		if (raw_build (&sp, &doc) < 0)
			return 4;

		mem = exif_mem_new_default ();
		d = load_doc (mem, doc.buf, doc.size);
		if (!d)
			return 5;
		if (collect_blocks (d, &blocks, &nblocks) < 0)
			return 6;

		for (cap = 100; cap <= 240; cap++) {
			struct pack_problem real;
			real.sizes = blocks;
			real.n = nblocks;
			real.note_start = note_off;
			real.note_end = even_up (note_off + 16);
			real.capacity = cap;
			if (!oracle_exhaustive (&real) ||
			    oracle_greedy (&real))
				continue;
			found = 1;
			if (exif_data_save_data_pinned (d, 6u + cap,
							&out, &size) != 0)
				return 7;
			if (size > 6u + cap)
				return 8;
			if (verify_tiff (out + 6, size - 6, &v) < 0 ||
			    v.note_off != note_off)
				return 9;
			break;
		}
		if (!found)
			return 10;

		free (blocks);
		exif_mem_free (mem, out);
		exif_data_unref (d);
		exif_mem_unref (mem);
		free (doc.buf);
	}
	return 0;
}

static int
case_exactly_full_and_impossible (void)
{
	/* Note pinned at offset 8 leaves no front region: every movable
	 * block lands behind it, so the output fills the capacity exactly. */
	const unsigned int note_off = 8;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[1];
	struct r_entry vals[2];
	unsigned char v6[6], v10[10];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned char *out = NULL;
	unsigned int size = 0;
	unsigned int *sizes = NULL, nsizes = 0;
	unsigned int total = 0, cap, i;
	struct verdict v;

	make_note (note, o, note_off);
	memset (v6, '6', sizeof (v6));
	memset (v10, 'a', sizeof (v10));

	vals[0].tag = (ExifTag) 0xc010;
	vals[0].format = EXIF_FORMAT_UNDEFINED;
	vals[0].components = 6;
	vals[0].value = v6;
	vals[0].size = 6;
	vals[0].valblk = 0;
	vals[1].tag = (ExifTag) 0xc011;
	vals[1].format = EXIF_FORMAT_UNDEFINED;
	vals[1].components = 10;
	vals[1].value = v10;
	vals[1].size = 10;
	vals[1].valblk = 0;
	note_fixture (&exif[0], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_0].entries = vals;
	sp.ifd[R_IFD_0].count = 2;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;
	if (collect_blocks (d, &sizes, &nsizes) < 0)
		return 3;
	for (i = 0; i < nsizes; i++)
		total += sizes[i];

	/* note_end (24) plus every movable block, all placed behind. */
	cap = even_up (note_off + 16u) + total;
	if (exif_data_save_data_pinned (d, 6u + cap, &out, &size) != 0)
		return 4;
	if (size != 6u + cap)
		return 5;
	if (verify_tiff (out + 6, size - 6, &v) < 0 ||
	    v.note_off != note_off)
		return 6;
	exif_mem_free (mem, out);
	out = NULL;

	/* One byte tighter: truly impossible. */
	if (exif_data_save_data_pinned (d, 6u + cap - 1u,
					&out, &size) == 0)
		return 7;
	if (out != NULL || size != 0)
		return 8;

	free (sizes);
	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_boundary_65533 (void)
{
	/* TIFF output is always even sized, so the largest payload that fits
	 * a 65533 byte budget is 65532. Pin the note at offset 8 (no front
	 * region) and size the only external value so the total lands on
	 * exactly that limit. */
	const unsigned int note_off = 8u;
	const unsigned int note_size = 16u;
	const unsigned int target_tiff = 65526u;	/* largest even <= cap */
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	ExifEntry *e;
	unsigned char *out = NULL;
	unsigned int size = 0;
	unsigned int value_len;
	struct verdict v;

	if (even_up (note_off + note_size) != 24u)
		return 1;
	/* Fixed geometry: note span 24, IFD0 directory 30 (value entry plus
	 * the synthesized Exif IFD pointer), Exif directory 18. */
	value_len = target_tiff - 24u - 30u - 18u;
	if (value_len & 1u)
		return 2;

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, note_size);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = note_size;
	if (raw_build (&sp, &doc) < 0)
		return 3;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 4;

	{
		unsigned char *fill = exif_mem_alloc (mem, value_len);
		if (!fill)
			return 5;
		memset (fill, 'F', value_len);
		e = exif_entry_new_mem (mem);
		if (!e)
			return 6;
		e->tag = (ExifTag) 0xc100;
		e->format = EXIF_FORMAT_UNDEFINED;
		e->components = value_len;
		e->data = fill;
		e->size = value_len;
		exif_content_add_entry (d->ifd[EXIF_IFD_0], e);
		exif_entry_unref (e);
	}

	if (exif_data_save_data_pinned (d, 65533, &out, &size) != 0)
		return 7;
	if (size != 6u + target_tiff || size > 65533)
		return 8;
	if (verify_tiff (out + 6, size - 6, &v) < 0)
		return 9;
	if (v.note_off != note_off ||
	    memcmp (out + 6 + note_off, note, note_size) != 0 ||
	    v.internal_target != note_off + 12)
		return 10;
	exif_mem_free (mem, out);
	out = NULL;

	/* The same layout exactly fills a 65532 byte budget ... */
	if (exif_data_save_data_pinned (d, 65532, &out, &size) != 0)
		return 11;
	if (size != 65532)
		return 12;
	exif_mem_free (mem, out);
	out = NULL;

	/* ... but one byte tighter is genuinely impossible. */
	if (exif_data_save_data_pinned (d, 65531, &out, &size) == 0)
		return 13;
	if (out != NULL || size != 0)
		return 14;

	/* Out of range budgets are rejected up front. */
	if (exif_data_save_data_pinned (d, 65534, &out, &size) == 0)
		return 15;
	if (out != NULL || size != 0)
		return 16;
	if (exif_data_save_data_pinned (d, 13, &out, &size) == 0)
		return 17;
	if (out != NULL || size != 0)
		return 18;

	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_retry_after_failure (void)
{
	const unsigned int note_off = 128;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	ExifEntry *de;
	unsigned char *out = NULL;
	unsigned int size = 0;
	const char *long_text =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	char short_text[12];

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;
	if (!add_ascii (mem, d, EXIF_IFD_0, EXIF_TAG_IMAGE_DESCRIPTION,
			long_text))
		return 3;

	/* Tight budget: the long description cannot fit. */
	if (exif_data_save_data_pinned (d, 200, &out, &size) == 0)
		return 4;
	if (out != NULL || size != 0)
		return 5;

	/* Shrink the description on the same object: save now works. */
	de = exif_content_get_entry (d->ifd[EXIF_IFD_0],
				     EXIF_TAG_IMAGE_DESCRIPTION);
	if (!de)
		return 6;
	memset (short_text, 'S', sizeof (short_text));
	short_text[sizeof (short_text) - 1] = 0;
	{
		unsigned char *nb = exif_mem_alloc (mem,
						    sizeof (short_text));
		if (!nb)
			return 7;
		memcpy (nb, short_text, sizeof (short_text));
		exif_mem_free (mem, de->data);
		de->data = nb;
		de->size = sizeof (short_text);
		de->components = sizeof (short_text);
	}
	if (exif_data_save_data_pinned (d, 200, &out, &size) != 0)
		return 8;
	if (!out || size == 0 || size > 200)
		return 9;

	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_inconsistent_entry_rejected (void)
{
	ExifMem *mem = exif_mem_new_default ();
	ExifData *d = exif_data_new_mem (mem);
	ExifEntry *e;
	unsigned char *out = (unsigned char *) 1;
	unsigned int size = 1;

	if (!d)
		return 1;
	e = exif_entry_new_mem (mem);
	if (!e)
		return 2;
	e->tag = (ExifTag) 0xc200;
	e->format = EXIF_FORMAT_SHORT;	/* 2 bytes per component */
	e->components = 10;		/* 20 bytes declared ... */
	e->data = exif_mem_alloc (mem, 10);
	if (!e->data)
		return 3;
	e->size = 10;			/* ... but only 10 available */
	exif_content_add_entry (d->ifd[EXIF_IFD_0], e);
	exif_entry_unref (e);

	if (exif_data_save_data_pinned (d, 65533, &out, &size) == 0)
		return 4;
	if (out != NULL || size != 0)
		return 5;

	exif_data_unref (d);
	exif_mem_unref (mem);
	return 0;
}

/* ============================================================ */
/* compatibility group                                          */
/* ============================================================ */

static int
case_no_note_compact (void)
{
	ExifMem *mem = exif_mem_new_default ();
	ExifData *d = exif_data_new_mem (mem);
	unsigned char *out = NULL;
	unsigned int size = 0;
	struct verdict v;
	ExifData *r;

	if (!d)
		return 1;
	if (!add_short_value (mem, d, EXIF_IFD_0,
			      EXIF_TAG_ORIENTATION, 3))
		return 2;
	if (!add_ascii (mem, d, EXIF_IFD_0, EXIF_TAG_SOFTWARE,
			"libexif-pin"))
		return 3;

	if (exif_data_save_data_pinned (d, 65533, &out, &size) != 0)
		return 4;
	if (verify_tiff (out + 6, size - 6, &v) < 0)
		return 5;
	if (v.note_found)
		return 6;

	r = exif_data_new_from_data (out, size);
	if (!r)
		return 7;
	if (!exif_content_get_entry (r->ifd[EXIF_IFD_0],
				     EXIF_TAG_ORIENTATION))
		return 8;
	exif_data_unref (r);
	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	return 0;
}

static int
case_thumbnail_roundtrip (void)
{
	const unsigned int note_off = 128;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	static const unsigned char thumb[] = {
		0xff, 0xd8, 0xff, 0xe0, 0x11, 0x22, 0x33, 0x44,
		0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xff, 0xd9
	};
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned char *out = NULL;
	unsigned int size = 0;
	struct verdict v;
	ExifData *r;

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	sp.thumb = thumb;
	sp.thumb_size = sizeof (thumb);
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d || !d->size || d->size != sizeof (thumb))
		return 2;

	if (exif_data_save_data_pinned (d, 65533, &out, &size) != 0)
		return 3;
	if (verify_tiff (out + 6, size - 6, &v) < 0)
		return 4;
	if (v.note_off != note_off || !v.thumb_found ||
	    v.thumb_size != sizeof (thumb))
		return 5;
	if (memcmp (out + 6 + v.thumb_off, thumb, sizeof (thumb)) != 0)
		return 6;

	r = exif_data_new_from_data (out, size);
	if (!r || r->size != sizeof (thumb) ||
	    memcmp (r->data, thumb, sizeof (thumb)) != 0)
		return 7;
	exif_data_unref (r);
	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_gps_and_fields_reload (void)
{
	const unsigned int note_off = 128;
	int pass;

	for (pass = 0; pass < 2; pass++) {
		ExifByteOrder o = pass ? EXIF_BYTE_ORDER_MOTOROLA :
					EXIF_BYTE_ORDER_INTEL;
		unsigned char note[16];
		struct r_entry exif[1];
		struct r_entry gps[2];
		unsigned char ref[2] = {'N', 0};
		ExifRational lat[3];
		struct build_spec sp;
		struct raw_doc doc;
		ExifMem *mem;
		ExifData *d, *r;
		unsigned char *out = NULL;
		unsigned int size = 0;
		struct verdict v;

		make_note (note, o, note_off);
		note_fixture (&exif[0], note, 16);

		lat[0].numerator = 1; lat[0].denominator = 2;
		lat[1].numerator = 3; lat[1].denominator = 4;
		lat[2].numerator = 5; lat[2].denominator = 6;
		gps[0].tag = EXIF_TAG_GPS_LATITUDE_REF;
		gps[0].format = EXIF_FORMAT_ASCII;
		gps[0].components = 2;
		gps[0].value = ref;
		gps[0].size = 2;
		gps[0].valblk = 0;
		gps[1].tag = EXIF_TAG_GPS_LATITUDE;
		gps[1].format = EXIF_FORMAT_RATIONAL;
		gps[1].components = 3;
		gps[1].value = (const unsigned char *) (const void *) lat;
		gps[1].size = sizeof (lat);
		gps[1].valblk = 0;

		memset (&sp, 0, sizeof (sp));
		sp.order = o;
		sp.ifd[R_IFD_EXIF].entries = exif;
		sp.ifd[R_IFD_EXIF].count = 1;
		sp.ifd[R_IFD_GPS].entries = gps;
		sp.ifd[R_IFD_GPS].count = 2;
		sp.note_offset = note_off;
		sp.note_bytes = note;
		sp.note_size = 16;
		if (raw_build (&sp, &doc) < 0)
			return 1;

		mem = exif_mem_new_default ();
		d = load_doc (mem, doc.buf, doc.size);
		if (!d)
			return 2;
		if (!add_short_value (mem, d, EXIF_IFD_0,
				      EXIF_TAG_ORIENTATION, 8))
			return 3;

		if (exif_data_save_data_pinned (d, 65533,
						&out, &size) != 0)
			return 4;
		if (verify_tiff (out + 6, size - 6, &v) < 0)
			return 5;
		if (v.note_off != note_off || !v.ifd[R_IFD_GPS].present)
			return 6;

		r = exif_data_new_from_data (out, size);
		if (!r)
			return 7;
		{
			ExifEntry *er = exif_content_get_entry (
				r->ifd[EXIF_IFD_GPS], EXIF_TAG_GPS_LATITUDE);
			ExifEntry *eo = exif_content_get_entry (
				r->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
			ExifEntry *mn = exif_content_get_entry (
				r->ifd[EXIF_IFD_EXIF], EXIF_TAG_MAKER_NOTE);
			if (!er || er->size != sizeof (lat) ||
			    memcmp (er->data, lat, sizeof (lat)) != 0)
				return 8;
			if (!eo || exif_get_short (eo->data, o) != 8)
				return 9;
			if (!mn || mn->size != 16)
				return 10;
		}
		exif_data_unref (r);
		exif_mem_free (mem, out);
		exif_data_unref (d);
		exif_mem_unref (mem);
		free (doc.buf);
	}
	return 0;
}

/* ============================================================ */
/* Allocation failure harness                                   */
/* ============================================================ */

static struct mem_trace {
	unsigned long allocs;
	unsigned long reallocs;
	long fail_at;	/* -1 = disabled */
} active_trace_storage;

static struct mem_trace *active_trace;

static void *
t_alloc (ExifLong s)
{
	if (active_trace->fail_at >= 0 &&
	    (long) active_trace->allocs >= active_trace->fail_at)
		return NULL;
	active_trace->allocs++;
	return calloc (1, (size_t) s);
}

static void *
t_realloc (void *p, ExifLong s)
{
	if (active_trace->fail_at >= 0 &&
	    (long) active_trace->reallocs >= active_trace->fail_at)
		return NULL;
	active_trace->reallocs++;
	return realloc (p, (size_t) s);
}

static void
t_free (void *p)
{
	if (p)
		free (p);
}

static int
case_allocation_failure (void)
{
	const unsigned int note_off = 128;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned char *out;
	unsigned int size;
	long threshold;
	int saw_failure = 0;

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	memset (&active_trace_storage, 0, sizeof (active_trace_storage));
	active_trace_storage.fail_at = -1;
	active_trace = &active_trace_storage;
	mem = exif_mem_new (t_alloc, t_realloc, t_free);
	if (!mem)
		return 2;
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 3;

	for (threshold = 0; threshold < 80; threshold++) {
		active_trace->fail_at = threshold;
		out = (unsigned char *) 1;
		size = 1;
		if (exif_data_save_data_pinned (d, 65533,
						 &out, &size) == 0) {
			/* Threshold beyond the save's allocation count: the
			 * run succeeded. */
			active_trace->fail_at = -1;
			if (!out || !size)
				return 4;
			t_free (out);
			break;
		}
		saw_failure = 1;
		if (out != NULL || size != 0)
			return 5;
	}
	active_trace->fail_at = -1;
	if (!saw_failure)
		return 6;

	/* The object remains fully usable after every failed attempt. */
	{
		struct verdict v;
		if (exif_data_save_data_pinned (d, 65533, &out, &size) != 0)
			return 7;
		if (verify_tiff (out + 6, size - 6, &v) < 0 ||
		    v.note_off != note_off)
			return 8;
		t_free (out);
	}

	exif_data_unref (d);
	exif_mem_unref (mem);
	active_trace = NULL;
	free (doc.buf);
	return 0;
}

static int
case_dp_packing_many_blocks (void)
{
	/* More than twenty movable blocks forces the subset-sum packer; the
	 * 0xc4xx tags are unknown to libexif and must survive untouched. */
	const unsigned int nval = 24;
	const unsigned int note_off = 400;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[1];
	struct r_entry vals[24];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned int i, trial;
	unsigned char fill[12];

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);
	memset (fill, 'Q', sizeof (fill));

	for (i = 0; i < nval; i++) {
		vals[i].tag = (ExifTag) (0xc400 + i);
		vals[i].format = EXIF_FORMAT_UNDEFINED;
		vals[i].components = 6u + (i % 7u);
		vals[i].value = fill;
		vals[i].size = 6u + (i % 7u);
		vals[i].valblk = 0;
	}

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_0].entries = vals;
	sp.ifd[R_IFD_0].count = nval;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;
	if (d->ifd[EXIF_IFD_0]->count != nval)
		return 3;	/* unknown tags must be retained */

	for (trial = 0; trial < 40; trial++) {
		unsigned char *out = NULL;
		unsigned int size = 0;
		unsigned int *blocks = NULL, nblocks = 0;
		unsigned int capacity = 520u + trial * 6u;
		struct pack_problem pp;
		int expected, got;
		struct verdict v;

		if (collect_blocks (d, &blocks, &nblocks) < 0)
			return 4;
		if (nblocks <= 20) {
			free (blocks);
			return 5;	/* DP path must be exercised */
		}
		pp.sizes = blocks;
		pp.n = nblocks;
		pp.note_start = note_off;
		pp.note_end = even_up (note_off + 16);
		pp.capacity = capacity;
		expected = oracle_subset_sum (&pp);
		got = (exif_data_save_data_pinned (d, 6u + capacity,
						    &out, &size) == 0);
		if (got != expected) {
			fprintf (stderr,
				 "dp mismatch trial=%u expected=%d got=%d\n",
				 trial, expected, got);
			free (blocks);
			return 6;
		}
		if (got) {
			if (verify_tiff (out + 6, size - 6, &v) < 0 ||
			    v.note_off != note_off) {
				free (blocks);
				return 7;
			}
			exif_mem_free (mem, out);
		}
		free (blocks);
	}

	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_interop_chain (void)
{
	const unsigned int note_off = 144;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	unsigned char idx[4] = {'R', '9', '8', 0};
	struct r_entry exif[1];
	struct r_entry interop[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d, *r;
	unsigned char *out = NULL;
	unsigned int size = 0;
	struct verdict v;

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);
	interop[0].tag = (ExifTag) 0x0001;	/* InteropIndex */
	interop[0].format = EXIF_FORMAT_ASCII;
	interop[0].components = sizeof (idx);
	interop[0].value = idx;
	interop[0].size = sizeof (idx);
	interop[0].valblk = 0;

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.ifd[R_IFD_INTEROP].entries = interop;
	sp.ifd[R_IFD_INTEROP].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d || !d->ifd[EXIF_IFD_INTEROPERABILITY]->count)
		return 2;
	if (exif_data_save_data_pinned (d, 65533, &out, &size) != 0)
		return 3;
	if (verify_tiff (out + 6, size - 6, &v) < 0)
		return 4;
	if (v.note_off != note_off || !v.ifd[R_IFD_INTEROP].present ||
	    !v.ifd[R_IFD_EXIF].present)
		return 5;

	r = exif_data_new_from_data (out, size);
	if (!r ||
	    !exif_content_get_entry (r->ifd[EXIF_IFD_INTEROPERABILITY],
				     (ExifTag) 0x0001))
		return 6;
	exif_data_unref (r);
	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_duplicate_note_rejected (void)
{
	const unsigned int note_off = 128;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[2];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned char *out = (unsigned char *) 1;
	unsigned int size = 1;

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);
	note_fixture (&exif[1], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 2;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;
	/* Two MakerNote records make the original position unprovable. */
	if (exif_data_save_data_pinned (d, 65533, &out, &size) == 0)
		return 3;
	if (out != NULL || size != 0)
		return 4;

	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_scratch_note_rejected (void)
{
	/* A MakerNote created on an object that never loaded a document has
	 * no provable original position and must be refused. */
	ExifMem *mem = exif_mem_new_default ();
	ExifData *d = exif_data_new_mem (mem);
	unsigned char note[16];
	unsigned char *out = (unsigned char *) 1;
	unsigned int size = 1;

	if (!d)
		return 1;
	memset (note, 0, sizeof (note));
	memcpy (note, "UFN0", 4);
	if (!add_undefined (mem, d, EXIF_IFD_EXIF, EXIF_TAG_MAKER_NOTE,
			    note, sizeof (note)))
		return 2;
	if (exif_data_save_data_pinned (d, 65533, &out, &size) == 0)
		return 3;
	if (out != NULL || size != 0)
		return 4;

	exif_data_unref (d);
	exif_mem_unref (mem);
	return 0;
}

static int
case_distinct_documents_distinct_pins (void)
{
	/* Each loaded object carries the pin of its own source document; a
	 * second object loaded from a second document pins to that document's
	 * offset rather than inheriting anything. */
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note_a[16], note_b[16];
	const unsigned int off_a = 128, off_b = 176;
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned char *out = NULL;
	unsigned int size = 0;
	struct verdict v;
	unsigned int pass;

	mem = exif_mem_new_default ();
	for (pass = 0; pass < 2; pass++) {
		unsigned int off = pass ? off_b : off_a;
		unsigned char *note = pass ? note_b : note_a;

		make_note (note, o, off);
		note_fixture (&exif[0], note, 16);
		memset (&sp, 0, sizeof (sp));
		sp.order = o;
		sp.ifd[R_IFD_EXIF].entries = exif;
		sp.ifd[R_IFD_EXIF].count = 1;
		sp.note_offset = off;
		sp.note_bytes = note;
		sp.note_size = 16;
		if (raw_build (&sp, &doc) < 0)
			return 1;

		d = load_doc (mem, doc.buf, doc.size);
		free (doc.buf);
		if (!d)
			return 2;
		if (exif_data_save_data_pinned (d, 65533, &out, &size) != 0)
			return 3;
		if (verify_tiff (out + 6, size - 6, &v) < 0 ||
		    v.note_off != off ||
		    memcmp (out + 6 + off, note, 16) != 0 ||
		    v.internal_target != off + 12)
			return 4;
		exif_mem_free (mem, out);
		exif_data_unref (d);
		out = NULL;
	}
	exif_mem_unref (mem);
	return 0;
}

static int
case_inline_note_compact (void)
{
	/* A MakerNote of at most four bytes has no internal addresses; the
	 * entry is saved inline like any other scalar rather than pinned. */
	ExifByteOrder o = EXIF_BYTE_ORDER_MOTOROLA;
	unsigned char four[4] = {'U', 'F', 'N', '4'};
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d, *r;
	unsigned char *out = NULL;
	unsigned int size = 0;
	struct verdict v;

	exif[0].tag = EXIF_TAG_MAKER_NOTE;
	exif[0].format = EXIF_FORMAT_UNDEFINED;
	exif[0].components = 4;
	exif[0].value = four;
	exif[0].size = 4;
	exif[0].valblk = 0;

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;
	if (exif_data_save_data_pinned (d, 65533, &out, &size) != 0)
		return 3;
	if (verify_tiff (out + 6, size - 6, &v) < 0)
		return 4;
	r = exif_data_new_from_data (out, size);
	if (!r)
		return 5;
	{
		ExifEntry *mn = exif_content_get_entry (
			r->ifd[EXIF_IFD_EXIF], EXIF_TAG_MAKER_NOTE);
		if (!mn || mn->size != 4 || memcmp (mn->data, four, 4) != 0)
			return 6;
	}
	exif_data_unref (r);
	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

static int
case_old_entry_behavior_unchanged (void)
{
	const unsigned int note_off = 128;
	ExifByteOrder o = EXIF_BYTE_ORDER_INTEL;
	unsigned char note[16];
	struct r_entry exif[1];
	struct build_spec sp;
	struct raw_doc doc;
	ExifMem *mem;
	ExifData *d;
	unsigned char *out = NULL;
	unsigned int size = 0;

	make_note (note, o, note_off);
	note_fixture (&exif[0], note, 16);

	memset (&sp, 0, sizeof (sp));
	sp.order = o;
	sp.ifd[R_IFD_EXIF].entries = exif;
	sp.ifd[R_IFD_EXIF].count = 1;
	sp.note_offset = note_off;
	sp.note_bytes = note;
	sp.note_size = 16;
	if (raw_build (&sp, &doc) < 0)
		return 1;

	mem = exif_mem_new_default ();
	d = load_doc (mem, doc.buf, doc.size);
	if (!d)
		return 2;

	/* The legacy writer still relocates the opaque block. */
	exif_data_save_data (d, &out, &size);
	if (!out || size < 30)
		return 3;
	{
		struct verdict v;
		if (verify_tiff (out + 6, size - 6, &v) < 0)
			return 4;
		if (!v.note_found)
			return 5;
		if (v.note_off == note_off)
			return 6;	/* legacy behavior must not pin */
		if (memcmp (out + 6 + v.note_off, note, 16) != 0)
			return 7;
	}

	exif_mem_free (mem, out);
	exif_data_unref (d);
	exif_mem_unref (mem);
	free (doc.buf);
	return 0;
}

/* ============================================================ */
/* Suite registration                                           */
/* ============================================================ */

const struct contract_case *
contract_cases (size_t *count)
{
	static const struct contract_case cases[] = {
		{"addresses", "ii_pinned_position",
		 case_addresses_ii},
		{"addresses", "mm_pinned_position",
		 case_addresses_mm},
		{"addresses", "note_near_tail_uses_front_space",
		 case_note_near_tail},
		{"addresses", "reject_byte_order_switch",
		 case_reject_byte_order_switch},
		{"addresses", "reject_tamper_then_recover",
		 case_reject_tamper_and_recover},
		{"addresses", "reject_remove_and_recreate",
		 case_reject_remove_recreate},
		{"addresses", "reject_scratch_note",
		 case_scratch_note_rejected},
		{"addresses", "unknown_signature_not_excluded",
		 case_unknown_signature_accepted},
		{"addresses", "default_options_still_pins",
		 case_default_options_pin},

		{"packing", "exhaustive_oracle_matches",
		 case_oracle_matches},
		{"packing", "greedy_failure_still_feasible",
		 case_greedy_vs_exhaustive},
		{"packing", "exactly_full_then_impossible",
		 case_exactly_full_and_impossible},
		{"packing", "boundary_65533",
		 case_boundary_65533},
		{"packing", "retry_after_failure",
		 case_retry_after_failure},
		{"packing", "inconsistent_entry_rejected",
		 case_inconsistent_entry_rejected},
		{"packing", "dp_packing_many_blocks",
		 case_dp_packing_many_blocks},

		{"compatibility", "no_note_compact_save",
		 case_no_note_compact},
		{"compatibility", "thumbnail_roundtrip",
		 case_thumbnail_roundtrip},
		{"compatibility", "gps_and_fields_reload_ii_mm",
		 case_gps_and_fields_reload},
		{"compatibility", "interop_chain",
		 case_interop_chain},
		{"compatibility", "duplicate_note_rejected",
		 case_duplicate_note_rejected},
		{"compatibility", "distinct_documents_distinct_pins",
		 case_distinct_documents_distinct_pins},
		{"compatibility", "allocation_failure",
		 case_allocation_failure},
		{"compatibility", "inline_note_compact",
		 case_inline_note_compact},
		{"compatibility", "old_entry_unchanged",
		 case_old_entry_behavior_unchanged},
	};
	*count = sizeof (cases) / sizeof (cases[0]);
	return cases;
}
