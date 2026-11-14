// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)
/*
 * libfdt - Flat Device Tree manipulation
 *
 * MMIO address translation reporting: resolve a device's reg entry
 * subrange through the chain of bus ranges up to the root (CPU)
 * address space.  Read-only, allocating, self-contained reports.
 */
#include "libfdt_env.h"

#include <fdt.h>
#include <libfdt.h>
#include <fdt_mmio.h>

#include "libfdt_internal.h"

/* Bus flavors */
enum {
	MMIO_BUS_NUMERIC,
	MMIO_BUS_PCI,
};

/* Internal space class for PCI configuration space: never appears in
 * reports, translation of config space is unsupported. */
#define MMIO_SPACE_CONFIG	UINT32_MAX

/* PCI phys.hi space select, bits 25:24 */
#define PCI_HI_SPACE_MASK	0x03000000U
#define PCI_HI_SPACE_IO		0x01000000U
#define PCI_HI_SPACE_MEM32	0x02000000U
#define PCI_HI_SPACE_MEM64	0x03000000U

struct mmio_bus {
	int node;
	int type;		/* MMIO_BUS_* */
	int addr_cells;
	int size_cells;
	uint64_t max_addr;	/* highest representable numeric address */
};

struct mmio_window {
	uint64_t child;		/* numeric child base */
	uint64_t parent;	/* numeric parent base */
	uint64_t size;
	uint32_t space;		/* child space class */
	uint32_t parent_space;	/* parent space class (PCI parent) */
	uint32_t parent_hi;	/* raw parent phys.hi (PCI parent) */
	int index;		/* entry index in the ranges property */
};

/* A fragment being translated; lv has room for one entry per path
 * level and is filled level by level as the fragment ascends. */
struct mmio_frag {
	uint64_t reg_offset;
	uint64_t start;
	uint64_t len;
	struct fdt_mmio_level *lv;
};

static int mmio_fail(struct fdt_mmio_error *err, int code, int node,
		     uint64_t reg_offset)
{
	if (err) {
		err->code = code;
		err->node = node;
		err->reg_offset = reg_offset;
	}
	return code;
}

/* Decode ncells big-endian cells; cells beyond the low 64 bits must be
 * zero so the value stays representable in uint64_t. */
static int mmio_read_cells(const fdt32_t *p, int ncells, uint64_t *val)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < ncells; i++) {
		uint32_t c = fdt32_to_cpu(p[i]);
		int shift = 32 * (ncells - 1 - i);

		if (shift >= 64) {
			if (c)
				return -FDT_MMIO_ERR_OVERFLOW;
		} else {
			v |= (uint64_t)c << shift;
		}
	}
	*val = v;
	return 0;
}

static uint32_t mmio_pci_space(uint32_t hi)
{
	switch (hi & PCI_HI_SPACE_MASK) {
	case PCI_HI_SPACE_IO:
		return FDT_MMIO_SPACE_PCI_IO;
	case PCI_HI_SPACE_MEM32:
		return FDT_MMIO_SPACE_PCI_MEM32;
	case PCI_HI_SPACE_MEM64:
		return FDT_MMIO_SPACE_PCI_MEM64;
	default:
		return MMIO_SPACE_CONFIG;
	}
}

/* Does the half-open interval [start, start+len) fit in a space whose
 * highest address is max_addr?  The end itself must stay representable
 * in uint64_t. */
static bool mmio_span_fits(uint64_t max_addr, uint64_t start, uint64_t len)
{
	uint64_t limit = max_addr;

	if (limit == UINT64_MAX)
		return len <= UINT64_MAX - start;
	limit++;	/* 32-bit space: 2^32, cannot wrap uint64_t */
	if (start > limit)
		return false;
	return len <= limit - start;
}

static int mmio_bus_info(const void *fdt, int node, struct mmio_bus *bus)
{
	const char *dtype;
	int ac, sc, len;

	bus->node = node;
	bus->type = MMIO_BUS_NUMERIC;
	dtype = fdt_getprop(fdt, node, "device_type", &len);
	if (dtype && len == 4 && !memcmp(dtype, "pci", 4))
		bus->type = MMIO_BUS_PCI;

	ac = fdt_address_cells(fdt, node);
	if (ac < 0)
		return ac;
	sc = fdt_size_cells(fdt, node);
	if (sc < 0)
		return sc;

	if (bus->type == MMIO_BUS_PCI) {
		/* PCI buses must use 3 address and 2 size cells */
		if (ac != 3 || sc != 2)
			return -FDT_MMIO_ERR_UNSUPPORTED;
		bus->max_addr = UINT64_MAX;
	} else {
		bus->max_addr = (ac == 1) ? UINT32_MAX : UINT64_MAX;
	}
	bus->addr_cells = ac;
	bus->size_cells = sc;
	return 0;
}

/* Parse reg entry reg_index of nodeoffset, whose parent bus is bus0.
 * Returns the numeric base, the size and, for PCI buses, the raw
 * phys.hi cell.  Does not reject PCI configuration space; callers
 * decide. */
static int mmio_read_reg(const void *fdt, int nodeoffset, int reg_index,
			 const struct mmio_bus *bus0,
			 uint64_t *basep, uint64_t *sizep, uint32_t *hip)
{
	const fdt32_t *reg, *entry;
	int len, entry_cells, count, ret;

	if (bus0->size_cells < 1)
		return -FDT_MMIO_ERR_UNSUPPORTED;

	reg = fdt_getprop(fdt, nodeoffset, "reg", &len);
	if (!reg)
		return len;
	entry_cells = bus0->addr_cells + bus0->size_cells;
	if (len % (entry_cells * (int)sizeof(fdt32_t)))
		return -FDT_ERR_BADVALUE;
	count = len / (entry_cells * (int)sizeof(fdt32_t));
	if (reg_index >= count)
		return -FDT_ERR_NOTFOUND;
	entry = reg + (size_t)reg_index * entry_cells;

	if (bus0->type == MMIO_BUS_PCI) {
		*hip = fdt32_to_cpu(entry[0]);
		*basep = ((uint64_t)fdt32_to_cpu(entry[1]) << 32) |
			 fdt32_to_cpu(entry[2]);
		ret = mmio_read_cells(entry + 3, 2, sizep);
	} else {
		*hip = 0;
		ret = mmio_read_cells(entry, bus0->addr_cells, basep);
		if (!ret)
			ret = mmio_read_cells(entry + bus0->addr_cells,
					      bus0->size_cells, sizep);
	}
	return ret;
}

static void mmio_free_frags(struct mmio_frag *frags, int nfrags)
{
	int i;

	if (!frags)
		return;
	for (i = 0; i < nfrags; i++)
		free(frags[i].lv);
	free(frags);
}

static int mmio_window_cmp(const void *a, const void *b)
{
	const struct mmio_window *wa = a;
	const struct mmio_window *wb = b;

	if (wa->space != wb->space)
		return wa->space < wb->space ? -1 : 1;
	if (wa->child != wb->child)
		return wa->child < wb->child ? -1 : 1;
	if (wa->index != wb->index)
		return wa->index < wb->index ? -1 : 1;
	return 0;
}

/* Parse the ranges property of cbus into a sorted window array. */
static int mmio_parse_windows(const struct mmio_bus *cbus,
			      const struct mmio_bus *pbus,
			      const fdt32_t *prop, int proplen,
			      struct mmio_window **windowsp, int *nwindowsp,
			      struct fdt_mmio_error *err, uint64_t err_off)
{
	struct mmio_window *windows = NULL;
	int entry_cells, nentries, nwindows = 0, i, ret;

	entry_cells = cbus->addr_cells + pbus->addr_cells + cbus->size_cells;
	if (proplen % (entry_cells * (int)sizeof(fdt32_t))) {
		ret = mmio_fail(err, -FDT_ERR_BADVALUE, cbus->node, err_off);
		goto fail;
	}
	nentries = proplen / (entry_cells * (int)sizeof(fdt32_t));
	if (nentries) {
		windows = malloc((size_t)nentries * sizeof(*windows));
		if (!windows) {
			ret = mmio_fail(err, -FDT_ERR_NOSPACE, cbus->node,
					err_off);
			goto fail;
		}
	}

	for (i = 0; i < nentries; i++) {
		const fdt32_t *p = prop + (size_t)i * entry_cells;
		const fdt32_t *pp = p + cbus->addr_cells;
		struct mmio_window w;
		int err2;

		w.index = i;
		w.parent_hi = 0;
		w.parent_space = FDT_MMIO_SPACE_MEMORY;

		/* child address, in this bus's space */
		if (cbus->type == MMIO_BUS_PCI) {
			uint32_t chi = fdt32_to_cpu(p[0]);

			w.space = mmio_pci_space(chi);
			if (w.space == MMIO_SPACE_CONFIG)
				continue;	/* unusable for MMIO */
			w.child = ((uint64_t)fdt32_to_cpu(p[1]) << 32) |
				  fdt32_to_cpu(p[2]);
		} else {
			w.space = FDT_MMIO_SPACE_MEMORY;
			err2 = mmio_read_cells(p, cbus->addr_cells, &w.child);
			if (err2)
				goto overflow;
		}

		/* length, in this bus's #size-cells */
		err2 = mmio_read_cells(pp + pbus->addr_cells,
				       cbus->size_cells, &w.size);
		if (err2)
			goto overflow;
		if (!w.size)
			continue;		/* covers nothing */
		if (!mmio_span_fits(cbus->max_addr, w.child, w.size))
			goto overflow;

		/* parent address, in the parent bus's space */
		if (pbus->type == MMIO_BUS_PCI) {
			w.parent_hi = fdt32_to_cpu(pp[0]);
			w.parent_space = mmio_pci_space(w.parent_hi);
			w.parent = ((uint64_t)fdt32_to_cpu(pp[1]) << 32) |
				   fdt32_to_cpu(pp[2]);
			if (!mmio_span_fits(UINT64_MAX, w.parent, w.size))
				goto overflow;
		} else {
			err2 = mmio_read_cells(pp, pbus->addr_cells,
					       &w.parent);
			if (err2)
				goto overflow;
			if (!mmio_span_fits(pbus->max_addr, w.parent, w.size))
				goto overflow;
		}

		windows[nwindows++] = w;
		continue;

	overflow:
		ret = mmio_fail(err, -FDT_MMIO_ERR_OVERFLOW, cbus->node,
				err_off);
		goto fail;
	}

	/* Overlapping child windows in the same space are ambiguous */
	qsort(windows, nwindows, sizeof(*windows), mmio_window_cmp);
	for (i = 1; i < nwindows; i++) {
		struct mmio_window *prev = &windows[i - 1];
		struct mmio_window *cur = &windows[i];

		if (cur->space == prev->space &&
		    cur->child < prev->child + prev->size) {
			ret = mmio_fail(err, -FDT_MMIO_ERR_OVERLAP,
					cbus->node, err_off);
			goto fail;
		}
	}

	*windowsp = windows;
	*nwindowsp = nwindows;
	return 0;

fail:
	free(windows);
	return ret;
}

/* Translate one fragment level up through a windowed ranges property,
 * splitting fragments at window boundaries. */
static int mmio_hop_ranges(const struct mmio_bus *cbus,
			   const struct mmio_bus *pbus,
			   const struct mmio_window *windows, int nwindows,
			   int nlevels, int level,
			   struct mmio_frag **fragsp, int *nfragsp,
			   struct fdt_mmio_error *err)
{
	struct mmio_frag *frags = *fragsp;
	struct mmio_frag *out = NULL;
	int nfrags = *nfragsp, nout = 0, cap = 0;
	int i, ret = 0;

	for (i = 0; i < nfrags; i++) {
		struct mmio_frag *f = &frags[i];
		uint64_t cur = f->start;
		uint64_t fend = f->start + f->len;
		uint32_t space = f->lv[level].space;

		while (cur < fend) {
			const struct mmio_window *w = NULL;
			struct mmio_frag piece;
			uint64_t plen, delta;
			int j, k;

			for (k = 0; k < nwindows; k++) {
				if (windows[k].space == space &&
				    windows[k].child + windows[k].size > cur) {
					w = &windows[k];
					break;
				}
			}
			if (!w || w->child > cur) {
				ret = mmio_fail(err, -FDT_MMIO_ERR_NO_MAPPING,
						cbus->node,
						f->reg_offset +
						(cur - f->start));
				goto fail;
			}
			if (pbus->type == MMIO_BUS_PCI &&
			    w->parent_space == MMIO_SPACE_CONFIG) {
				ret = mmio_fail(err,
						-FDT_MMIO_ERR_UNSUPPORTED,
						cbus->node,
						f->reg_offset +
						(cur - f->start));
				goto fail;
			}

			plen = w->child + w->size - cur;
			if (plen > fend - cur)
				plen = fend - cur;

			if (nout == cap) {
				int ncap = cap ? cap * 2 : 8;
				struct mmio_frag *tmp;

				tmp = realloc(out,
					      (size_t)ncap * sizeof(*out));
				if (!tmp) {
					ret = mmio_fail(err, -FDT_ERR_NOSPACE,
							cbus->node,
							f->reg_offset);
					goto fail;
				}
				out = tmp;
				cap = ncap;
			}

			piece.reg_offset = f->reg_offset + (cur - f->start);
			piece.start = w->parent + (cur - w->child);
			piece.len = plen;
			piece.lv = malloc((size_t)nlevels *
					  sizeof(*piece.lv));
			if (!piece.lv) {
				ret = mmio_fail(err, -FDT_ERR_NOSPACE,
						cbus->node, f->reg_offset);
				goto fail;
			}
			memcpy(piece.lv, f->lv,
			       (size_t)(level + 1) * sizeof(*piece.lv));

			/* The split shifts the span at every level so
			 * far by the same delta; lengths are preserved
			 * by translation. */
			delta = cur - f->start;
			for (j = 0; j <= level; j++) {
				piece.lv[j].start += delta;
				piece.lv[j].end = piece.lv[j].start + plen;
			}
			piece.lv[level].ranges_index = w->index;
			piece.lv[level + 1].start = piece.start;
			piece.lv[level + 1].end = piece.start + plen;
			if (pbus->type == MMIO_BUS_PCI) {
				piece.lv[level + 1].space = w->parent_space;
				piece.lv[level + 1].pci_hi = w->parent_hi;
			} else {
				piece.lv[level + 1].space =
					FDT_MMIO_SPACE_MEMORY;
				piece.lv[level + 1].pci_hi = 0;
			}
			piece.lv[level + 1].ranges_index = FDT_MMIO_RANGES_NONE;

			out[nout++] = piece;
			cur += plen;
		}
	}

	/* the pieces hold private copies of the level records */
	mmio_free_frags(frags, nfrags);
	*fragsp = out;
	*nfragsp = nout;
	return 0;

fail:
	mmio_free_frags(out, nout);
	return ret;
}

/* Translate one fragment level up through an empty ranges property
 * (identity mapping).  Only same-type buses can pass a space through
 * without dropping its type. */
static int mmio_hop_identity(const struct mmio_bus *cbus,
			     const struct mmio_bus *pbus,
			     int level,
			     struct mmio_frag *frags, int nfrags,
			     struct fdt_mmio_error *err)
{
	int i;

	if (cbus->type != pbus->type)
		return mmio_fail(err, -FDT_MMIO_ERR_UNSUPPORTED, cbus->node,
				 frags[0].reg_offset);

	for (i = 0; i < nfrags; i++) {
		struct mmio_frag *f = &frags[i];

		if (!mmio_span_fits(pbus->max_addr, f->start, f->len))
			return mmio_fail(err, -FDT_MMIO_ERR_OVERFLOW,
					 cbus->node, f->reg_offset);
		f->lv[level].ranges_index = FDT_MMIO_RANGES_NONE;
		f->lv[level + 1] = f->lv[level];
		f->lv[level + 1].ranges_index = FDT_MMIO_RANGES_NONE;
	}
	return 0;
}

static char *mmio_get_path(const void *fdt, int node)
{
	size_t cap = 64;
	char *buf;
	int ret;

	for (;;) {
		buf = malloc(cap);
		if (!buf)
			return NULL;
		ret = fdt_get_path(fdt, node, buf, (int)cap);
		if (!ret)
			return buf;
		free(buf);
		if (ret != -FDT_ERR_NOSPACE)
			return NULL;
		cap *= 2;
		if (cap > 65536)
			return NULL;
	}
}

/* Assemble the self-contained report in a single allocation. */
static struct fdt_mmio_report *mmio_build_report(const void *fdt,
						 const struct mmio_bus *buses,
						 int nbuses,
						 struct mmio_frag *frags,
						 int nfrags)
{
	struct fdt_mmio_report *report;
	struct fdt_mmio_segment *segments;
	struct fdt_mmio_bus *rep_buses;
	struct fdt_mmio_level *levels;
	char *strings, *p;
	size_t sz, str_sz = 0;
	char **paths;
	int i, j;

	paths = malloc((size_t)nbuses * sizeof(*paths));
	if (!paths)
		return NULL;
	for (i = 0; i < nbuses; i++) {
		paths[i] = mmio_get_path(fdt, buses[i].node);
		if (!paths[i]) {
			for (j = 0; j < i; j++)
				free(paths[j]);
			free(paths);
			return NULL;
		}
		str_sz += strlen(paths[i]) + 1;
	}

	sz = sizeof(*report) +
	     (size_t)nfrags * sizeof(*segments) +
	     (size_t)nbuses * sizeof(*rep_buses) +
	     (size_t)nfrags * nbuses * sizeof(*levels) +
	     str_sz;
	report = calloc(1, sz);
	if (!report) {
		for (i = 0; i < nbuses; i++)
			free(paths[i]);
		free(paths);
		return NULL;
	}

	segments = (struct fdt_mmio_segment *)(report + 1);
	rep_buses = (struct fdt_mmio_bus *)(segments + nfrags);
	levels = (struct fdt_mmio_level *)(rep_buses + nbuses);
	strings = (char *)(levels + (size_t)nfrags * nbuses);

	report->n_segments = nfrags;
	report->n_levels = nbuses;
	report->segments = nfrags ? segments : NULL;
	report->buses = nbuses ? rep_buses : NULL;

	p = strings;
	for (j = 0; j < nbuses; j++) {
		size_t len = strlen(paths[j]) + 1;

		rep_buses[j].node = buses[j].node;
		rep_buses[j].addr_cells = buses[j].addr_cells;
		rep_buses[j].path = p;
		memcpy(p, paths[j], len);
		p += len;
		free(paths[j]);
	}
	free(paths);

	for (i = 0; i < nfrags; i++) {
		struct fdt_mmio_level *lv = levels + (size_t)i * nbuses;

		memcpy(lv, frags[i].lv,
		       (size_t)nbuses * sizeof(*lv));
		segments[i].reg_offset = frags[i].reg_offset;
		segments[i].length = frags[i].len;
		segments[i].cpu_address = lv[nbuses - 1].start;
		segments[i].levels = lv;
	}
	return report;
}

static struct fdt_mmio_report *mmio_empty_report(void)
{
	return calloc(1, sizeof(struct fdt_mmio_report));
}

int fdt_mmio_translate(const void *fdt, int nodeoffset, int reg_index,
		       uint64_t offset, uint64_t length,
		       struct fdt_mmio_report **reportp,
		       struct fdt_mmio_error *error)
{
	struct mmio_bus *buses = NULL;
	struct mmio_frag *frags = NULL;
	struct fdt_mmio_report *report = NULL;
	uint64_t base, size;
	uint32_t hi = 0, space0;
	int nbuses = 0, nfrags = 0, nbus_cap = 0;
	int i, ret, parent, cur;

	if (!reportp)
		return -FDT_ERR_BADVALUE;
	*reportp = NULL;
	if (!can_assume(VALID_DTB)) {
		int32_t totalsize = fdt_ro_probe_(fdt);

		if (totalsize < 0)
			return mmio_fail(error, totalsize, -1, offset);
	}
	if (reg_index < 0)
		return mmio_fail(error, -FDT_ERR_BADVALUE, -1, 0);

	/* The device's own bus interprets its reg property */
	parent = fdt_parent_offset(fdt, nodeoffset);
	if (parent < 0)
		return mmio_fail(error, parent, -1, offset);

	buses = malloc(sizeof(*buses));
	if (!buses)
		return mmio_fail(error, -FDT_ERR_NOSPACE, -1, offset);
	nbus_cap = 1;
	ret = mmio_bus_info(fdt, parent, &buses[0]);
	if (ret) {
		ret = mmio_fail(error, ret, parent, offset);
		goto fail;
	}
	nbuses = 1;

	ret = mmio_read_reg(fdt, nodeoffset, reg_index, &buses[0],
			    &base, &size, &hi);
	if (ret) {
		ret = mmio_fail(error, ret, parent, offset);
		goto fail;
	}

	/* The entry's own half-open end must be representable */
	if (size > UINT64_MAX - base) {
		ret = mmio_fail(error, -FDT_MMIO_ERR_OVERFLOW, parent, offset);
		goto fail;
	}
	if (offset > size || length > size - offset) {
		ret = mmio_fail(error, -FDT_ERR_BADVALUE, parent, offset);
		goto fail;
	}
	if (!mmio_span_fits(buses[0].max_addr, base + offset, length)) {
		ret = mmio_fail(error, -FDT_MMIO_ERR_OVERFLOW, parent, offset);
		goto fail;
	}

	if (buses[0].type == MMIO_BUS_PCI) {
		space0 = mmio_pci_space(hi);
		if (space0 == MMIO_SPACE_CONFIG) {
			ret = mmio_fail(error, -FDT_MMIO_ERR_UNSUPPORTED,
					parent, offset);
			goto fail;
		}
	} else {
		space0 = FDT_MMIO_SPACE_MEMORY;
	}

	if (!length) {
		/* Valid but empty request: empty report */
		report = mmio_empty_report();
		if (!report)
			ret = mmio_fail(error, -FDT_ERR_NOSPACE, parent,
					offset);
		else
			ret = 0;
		goto out;
	}

	/* Collect the rest of the bus path up to the root */
	cur = parent;
	while (cur != 0) {
		cur = fdt_parent_offset(fdt, cur);
		if (cur < 0) {
			ret = mmio_fail(error, cur, -1, offset);
			goto fail;
		}
		if (nbuses == nbus_cap) {
			struct mmio_bus *tmp;

			tmp = realloc(buses,
				      (size_t)nbus_cap * 2 * sizeof(*buses));
			if (!tmp) {
				ret = mmio_fail(error, -FDT_ERR_NOSPACE, -1,
						offset);
				goto fail;
			}
			buses = tmp;
			nbus_cap *= 2;
		}
		ret = mmio_bus_info(fdt, cur, &buses[nbuses]);
		if (ret) {
			ret = mmio_fail(error, ret, cur, offset);
			goto fail;
		}
		nbuses++;
	}

	/* Start with a single fragment in the device's bus space */
	frags = malloc(sizeof(*frags));
	if (!frags) {
		ret = mmio_fail(error, -FDT_ERR_NOSPACE, parent, offset);
		goto fail;
	}
	frags[0].lv = calloc((size_t)nbuses, sizeof(*frags[0].lv));
	if (!frags[0].lv) {
		ret = mmio_fail(error, -FDT_ERR_NOSPACE, parent, offset);
		goto fail;
	}
	nfrags = 1;
	frags[0].reg_offset = offset;
	frags[0].start = base + offset;
	frags[0].len = length;
	frags[0].lv[0].start = base + offset;
	frags[0].lv[0].end = base + offset + length;
	frags[0].lv[0].space = space0;
	frags[0].lv[0].pci_hi = hi;
	frags[0].lv[0].ranges_index = FDT_MMIO_RANGES_NONE;

	/* Walk up one bus at a time; every hop must cover everything */
	for (i = 0; i + 1 < nbuses; i++) {
		const struct mmio_bus *cbus = &buses[i];
		const struct mmio_bus *pbus = &buses[i + 1];
		const fdt32_t *ranges;
		int rlen;

		if (cbus->size_cells < 1) {
			ret = mmio_fail(error, -FDT_MMIO_ERR_UNSUPPORTED,
					cbus->node, frags[0].reg_offset);
			goto fail;
		}

		ranges = fdt_getprop(fdt, cbus->node, "ranges", &rlen);
		if (!ranges) {
			if (rlen == -FDT_ERR_NOTFOUND)
				ret = mmio_fail(error,
						-FDT_MMIO_ERR_NO_MAPPING,
						cbus->node,
						frags[0].reg_offset);
			else
				ret = mmio_fail(error, rlen, cbus->node,
						frags[0].reg_offset);
			goto fail;
		}

		if (!rlen) {
			ret = mmio_hop_identity(cbus, pbus, i, frags, nfrags,
						error);
		} else {
			struct mmio_window *windows = NULL;
			int nwindows = 0;

			ret = mmio_parse_windows(cbus, pbus, ranges, rlen,
						 &windows, &nwindows, error,
						 frags[0].reg_offset);
			if (!ret)
				ret = mmio_hop_ranges(cbus, pbus, windows,
						      nwindows, nbuses, i,
						      &frags, &nfrags, error);
			free(windows);
		}
		if (ret)
			goto fail;
	}

	report = mmio_build_report(fdt, buses, nbuses, frags, nfrags);
	if (!report)
		ret = mmio_fail(error, -FDT_ERR_NOSPACE, -1, offset);
	else
		ret = 0;

out:
	if (!ret)
		*reportp = report;
fail:
	if (ret)
		free(report);
	mmio_free_frags(frags, nfrags);
	free(buses);
	return ret;
}

void fdt_mmio_report_free(struct fdt_mmio_report *report)
{
	/* single allocation, including paths and level storage */
	free(report);
}

int fdt_mmio_reg_entry(const void *fdt, int nodeoffset, int reg_index,
		       uint64_t *addressp, uint64_t *sizep)
{
	struct mmio_bus bus0;
	uint64_t base, size;
	uint32_t hi;
	int parent, ret;

	FDT_RO_PROBE(fdt);
	if (reg_index < 0)
		return -FDT_ERR_BADVALUE;
	parent = fdt_parent_offset(fdt, nodeoffset);
	if (parent < 0)
		return parent;
	ret = mmio_bus_info(fdt, parent, &bus0);
	if (ret)
		return ret;
	ret = mmio_read_reg(fdt, nodeoffset, reg_index, &bus0,
			    &base, &size, &hi);
	if (ret)
		return ret;
	if (addressp)
		*addressp = base;
	if (sizep)
		*sizep = size;
	return 0;
}

static void mmio_free_items(struct fdt_mmio_audit_item *items, uint32_t n)
{
	uint32_t i;
	int j;

	if (!items)
		return;
	for (i = 0; i < n; i++) {
		free(items[i].levels);
		if (items[i].buses) {
			for (j = 0; j < (int)items[i].n_levels; j++)
				free(items[i].buses[j].path);
			free(items[i].buses);
		}
	}
	free(items);
}

static int mmio_audit_unknown(struct fdt_mmio_audit_item **itemsp,
			      uint32_t *nitemsp, uint32_t *capp,
			      int reg_index, uint64_t reg_offset,
			      int error_code, int error_node)
{
	struct fdt_mmio_audit_item *item;

	if (*nitemsp == *capp) {
		uint32_t ncap = *capp ? *capp * 2 : 8;
		struct fdt_mmio_audit_item *tmp;

		tmp = realloc(*itemsp, (size_t)ncap * sizeof(*tmp));
		if (!tmp)
			return -FDT_ERR_NOSPACE;
		*itemsp = tmp;
		*capp = ncap;
	}
	item = &(*itemsp)[(*nitemsp)++];
	memset(item, 0, sizeof(*item));
	item->verdict = FDT_MMIO_VERDICT_UNKNOWN;
	item->reg_index = reg_index;
	item->reg_offset = reg_offset;
	item->error_code = error_code;
	item->error_node = error_node;
	return 0;
}

/* Append one allowed/violation item for [cpu_start, cpu_end). */
static int mmio_audit_piece(struct fdt_mmio_audit_item **itemsp,
			    uint32_t *nitemsp, uint32_t *capp,
			    const struct fdt_mmio_report *report,
			    const struct fdt_mmio_segment *seg,
			    uint32_t verdict, int reg_index,
			    uint64_t cpu_start, uint64_t cpu_end)
{
	struct fdt_mmio_audit_item *item;
	uint32_t nl = report->n_levels;
	uint32_t j;
	int ret;

	ret = mmio_audit_unknown(itemsp, nitemsp, capp, reg_index, 0, 0, -1);
	if (ret)
		return ret;
	item = &(*itemsp)[*nitemsp - 1];
	item->verdict = verdict;
	item->reg_offset = seg->reg_offset + (cpu_start - seg->cpu_address);
	item->cpu_start = cpu_start;
	item->cpu_end = cpu_end;
	item->n_levels = nl;

	item->levels = malloc((size_t)nl * sizeof(*item->levels));
	item->buses = calloc((size_t)nl, sizeof(*item->buses));
	if (!item->levels || !item->buses)
		return -FDT_ERR_NOSPACE;
	memcpy(item->levels, seg->levels, (size_t)nl * sizeof(*item->levels));
	for (j = 0; j < nl; j++) {
		size_t len = strlen(report->buses[j].path) + 1;

		item->buses[j].node = report->buses[j].node;
		item->buses[j].addr_cells = report->buses[j].addr_cells;
		item->buses[j].path = malloc(len);
		if (!item->buses[j].path)
			return -FDT_ERR_NOSPACE;
		memcpy(item->buses[j].path, report->buses[j].path, len);
	}
	return 0;
}

/* Classify one translated segment against the allowed intervals. */
static int mmio_audit_segment(struct fdt_mmio_audit_item **itemsp,
			      uint32_t *nitemsp, uint32_t *capp,
			      const struct fdt_mmio_report *report,
			      const struct fdt_mmio_segment *seg,
			      int reg_index,
			      const struct fdt_mmio_allow_range *allowed,
			      uint32_t n_allowed)
{
	uint64_t pos = seg->cpu_address;
	uint64_t end = seg->cpu_address + seg->length;

	while (pos < end) {
		uint64_t cov_end = 0, next = end, pe;
		uint32_t k;
		bool covered = false;

		for (k = 0; k < n_allowed; k++) {
			if (allowed[k].start <= pos && pos < allowed[k].end) {
				covered = true;
				if (allowed[k].end > cov_end)
					cov_end = allowed[k].end;
			}
		}
		if (covered) {
			pe = cov_end < end ? cov_end : end;
		} else {
			for (k = 0; k < n_allowed; k++)
				if (allowed[k].start > pos &&
				    allowed[k].start < next)
					next = allowed[k].start;
			pe = next;
		}

		int ret = mmio_audit_piece(itemsp, nitemsp, capp, report, seg,
					   covered ? FDT_MMIO_VERDICT_ALLOWED
						   : FDT_MMIO_VERDICT_VIOLATION,
					   reg_index, pos, pe);
		if (ret)
			return ret;
		pos = pe;
	}
	return 0;
}

int fdt_mmio_audit(const void *fdt, int nodeoffset,
		   const struct fdt_mmio_allow_range *allowed,
		   uint32_t n_allowed,
		   struct fdt_mmio_audit **auditp)
{
	struct fdt_mmio_audit_item *items = NULL;
	struct fdt_mmio_audit *audit = NULL;
	uint32_t nitems = 0, cap = 0, verdict = FDT_MMIO_VERDICT_ALLOWED;
	uint32_t i, k;
	int reg_index, ret;

	if (!auditp)
		return -FDT_ERR_BADVALUE;
	*auditp = NULL;
	FDT_RO_PROBE(fdt);
	if (!allowed && n_allowed)
		return -FDT_ERR_BADVALUE;
	for (k = 0; k < n_allowed; k++)
		if (allowed[k].start > allowed[k].end)
			return -FDT_ERR_BADVALUE;

	for (reg_index = 0; ; reg_index++) {
		struct fdt_mmio_report *report = NULL;
		struct fdt_mmio_error error;
		uint64_t size;
		uint32_t s;

		ret = fdt_mmio_reg_entry(fdt, nodeoffset, reg_index,
					 NULL, &size);
		if (ret == -FDT_ERR_NOTFOUND)
			break;
		if (ret) {
			/* Cannot reliably enumerate past this point */
			ret = mmio_audit_unknown(&items, &nitems, &cap,
						 reg_index, 0, ret, -1);
			if (ret)
				goto fail;
			break;
		}
		if (!size)
			continue;

		ret = fdt_mmio_translate(fdt, nodeoffset, reg_index, 0, size,
					 &report, &error);
		if (ret) {
			ret = mmio_audit_unknown(&items, &nitems, &cap,
						 reg_index, error.reg_offset,
						 ret, error.node);
			if (ret)
				goto fail;
			continue;
		}
		for (s = 0; s < report->n_segments; s++) {
			ret = mmio_audit_segment(&items, &nitems, &cap, report,
						 &report->segments[s],
						 reg_index, allowed,
						 n_allowed);
			if (ret) {
				fdt_mmio_report_free(report);
				goto fail;
			}
		}
		fdt_mmio_report_free(report);
	}

	for (i = 0; i < nitems; i++) {
		if (items[i].verdict == FDT_MMIO_VERDICT_VIOLATION)
			verdict = FDT_MMIO_VERDICT_VIOLATION;
	}
	if (verdict != FDT_MMIO_VERDICT_VIOLATION)
		for (i = 0; i < nitems; i++)
			if (items[i].verdict == FDT_MMIO_VERDICT_UNKNOWN)
				verdict = FDT_MMIO_VERDICT_UNKNOWN;

	audit = malloc(sizeof(*audit));
	if (!audit) {
		ret = -FDT_ERR_NOSPACE;
		goto fail;
	}
	audit->verdict = verdict;
	audit->n_items = nitems;
	audit->items = items;
	*auditp = audit;
	return 0;

fail:
	mmio_free_items(items, nitems);
	return ret;
}

void fdt_mmio_audit_free(struct fdt_mmio_audit *audit)
{
	if (!audit)
		return;
	mmio_free_items(audit->items, audit->n_items);
	free(audit);
}
