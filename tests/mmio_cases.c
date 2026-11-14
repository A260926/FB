// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)
/*
 * Test cases for the libfdt MMIO translation reporting API
 * (libfdt/fdt_mmio.h).  Small trees are synthesized with the libfdt
 * write interface; the expected physical spans are written out
 * independently of the implementation.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfdt.h>
#include <fdt_mmio.h>

#include "mmio_case.h"

#define BLOB_SIZE	8192

/* Append v as n big-endian cells (most significant cell first). */
static fdt32_t *put_cells(fdt32_t *p, int n, uint64_t v)
{
	int i;

	for (i = n - 1; i >= 0; i--) {
		p[i] = cpu_to_fdt32((uint32_t)(v & 0xffffffffU));
		v >>= 32;
	}
	return p + n;
}

static int prop_cells(void *fdt, int node, const char *name, fdt32_t *cells,
		      int ncells)
{
	return fdt_setprop(fdt, node, name, cells,
			   (int)(ncells * sizeof(fdt32_t)));
}

static int new_tree(char *buf, int root_ac, int root_sc)
{
	int ret;

	ret = fdt_create_empty_tree(buf, BLOB_SIZE);
	if (ret)
		return ret;
	ret = fdt_setprop_u32(buf, 0, "#address-cells", root_ac);
	if (ret)
		return ret;
	return fdt_setprop_u32(buf, 0, "#size-cells", root_sc);
}

/* Check one segment's offsets and its per-level spans. */
static int expect_seg(unsigned *checks, const struct fdt_mmio_report *rep,
		      int idx, uint64_t reg_off, uint64_t len, uint64_t cpu)
{
	const struct fdt_mmio_segment *seg;

	MMIO_CHECK(idx < (int)rep->n_segments);
	seg = &rep->segments[idx];
	MMIO_CHECK(seg->reg_offset == reg_off);
	MMIO_CHECK(seg->length == len);
	MMIO_CHECK(seg->cpu_address == cpu);
	MMIO_CHECK(seg->levels != NULL);
	return 0;
}

static int expect_level(unsigned *checks, const struct fdt_mmio_segment *seg,
			int lvl, uint64_t start, uint64_t end, uint32_t space,
			int ranges_index)
{
	const struct fdt_mmio_level *l = &seg->levels[lvl];

	MMIO_CHECK(l->start == start);
	MMIO_CHECK(l->end == end);
	MMIO_CHECK(l->space == space);
	MMIO_CHECK(l->ranges_index == ranges_index);
	return 0;
}

/*
 * nested: three bus levels with different cell widths (root 2/2,
 * bus-a 3/2, bus-b 1/1), two windows in one hop, plus a 4-cell bus.
 */
static int check_nested(unsigned *checks)
{
	char blob[BLOB_SIZE];
	fdt32_t cells[16], *p;
	int busa, busb, dev, ret;
	struct fdt_mmio_report *rep = NULL;
	struct fdt_mmio_error err;

	MMIO_CHECK(new_tree(blob, 2, 2) == 0);

	busa = fdt_add_subnode(blob, 0, "bus-a");
	MMIO_CHECK(busa >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, busa, "#address-cells", 3) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, busa, "#size-cells", 2) == 0);
	/* child [0x1_00000000, +0x10000) -> root [0x80000000, +0x10000) */
	p = cells;
	p = put_cells(p, 3, 0x100000000ULL);
	p = put_cells(p, 2, 0x80000000ULL);
	p = put_cells(p, 2, 0x10000ULL);
	MMIO_CHECK(prop_cells(blob, busa, "ranges", cells,
			      (int)(p - cells)) == 0);

	busb = fdt_add_subnode(blob, busa, "bus-b");
	MMIO_CHECK(busb >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, busb, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, busb, "#size-cells", 1) == 0);
	/* w0: [0x18000,+0x4000) -> [0x1_00001000,+0x4000)
	 * w1: [0x20000,+0x1000) -> [0x1_00008000,+0x1000) */
	p = cells;
	p = put_cells(p, 1, 0x18000);
	p = put_cells(p, 3, 0x100001000ULL);
	p = put_cells(p, 1, 0x4000);
	p = put_cells(p, 1, 0x20000);
	p = put_cells(p, 3, 0x100008000ULL);
	p = put_cells(p, 1, 0x1000);
	MMIO_CHECK(prop_cells(blob, busb, "ranges", cells,
			      (int)(p - cells)) == 0);

	dev = fdt_add_subnode(blob, busb, "dev");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x18080);
	p = put_cells(p, 1, 0x100);
	p = put_cells(p, 1, 0x20400);
	p = put_cells(p, 1, 0x80);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);

	/* entry 0: [0x18080,+0x100) -> [0x100001080,..) -> [0x80001080,..) */
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x100, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(rep != NULL);
	MMIO_CHECK(rep->n_segments == 1);
	MMIO_CHECK(rep->n_levels == 3);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x100, 0x80001080ULL) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 0, 0x18080,
				0x18180, FDT_MMIO_SPACE_MEMORY, 0) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 1,
				0x100001080ULL, 0x100001180ULL,
				FDT_MMIO_SPACE_MEMORY, 0) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 2, 0x80001080ULL,
				0x80001180ULL, FDT_MMIO_SPACE_MEMORY,
				FDT_MMIO_RANGES_NONE) == 0);
	MMIO_CHECK(rep->buses[0].addr_cells == 1);
	MMIO_CHECK(rep->buses[1].addr_cells == 3);
	MMIO_CHECK(rep->buses[2].addr_cells == 2);
	MMIO_CHECK(!strcmp(rep->buses[0].path, "/bus-a/bus-b"));
	MMIO_CHECK(!strcmp(rep->buses[1].path, "/bus-a"));
	MMIO_CHECK(!strcmp(rep->buses[2].path, "/"));
	fdt_mmio_report_free(rep);
	rep = NULL;

	/* subrange of entry 0 */
	ret = fdt_mmio_translate(blob, dev, 0, 0x40, 0x20, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(rep->n_segments == 1);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0x40, 0x20, 0x800010c0ULL) == 0);
	fdt_mmio_report_free(rep);
	rep = NULL;

	/* entry 1 takes the second window of bus-b's ranges */
	ret = fdt_mmio_translate(blob, dev, 1, 0, 0x80, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(rep->n_segments == 1);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x80, 0x80008400ULL) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 0, 0x20400,
				0x20480, FDT_MMIO_SPACE_MEMORY, 1) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 1,
				0x100008400ULL, 0x100008480ULL,
				FDT_MMIO_SPACE_MEMORY, 0) == 0);
	fdt_mmio_report_free(rep);
	rep = NULL;

	/* a bus with 4 address cells: high cells must be zero */
	MMIO_CHECK(new_tree(blob, 2, 2) == 0);
	busa = fdt_add_subnode(blob, 0, "bus-q");
	MMIO_CHECK(busa >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, busa, "#address-cells", 4) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, busa, "#size-cells", 2) == 0);
	p = cells;
	p = put_cells(p, 4, 0x2000);
	p = put_cells(p, 2, 0x60000000ULL);
	p = put_cells(p, 2, 0x1000);
	MMIO_CHECK(prop_cells(blob, busa, "ranges", cells,
			      (int)(p - cells)) == 0);
	dev = fdt_add_subnode(blob, busa, "dev-ok");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 4, 0x2400);
	p = put_cells(p, 2, 0x100);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x100, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x100, 0x60000400ULL) == 0);
	fdt_mmio_report_free(rep);
	rep = NULL;

	/* same bus, reg address with nonzero high cells: overflow */
	dev = fdt_add_subnode(blob, busa, "dev-bad");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 1);		/* nonzero cell above 64 bits */
	p = put_cells(p, 3, 0);
	p = put_cells(p, 2, 0x100);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x100, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_OVERFLOW);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == busa);
	MMIO_CHECK(err.reg_offset == 0);

	return 0;
}

/*
 * split: one reg range is cut by window boundaries at two consecutive
 * hops, producing three segments; audit pieces fall across two allowed
 * windows with a forbidden zone in between.
 */
static int check_split(unsigned *checks)
{
	char blob[BLOB_SIZE];
	fdt32_t cells[24], *p;
	int busm, busl, dev, ret;
	struct fdt_mmio_report *rep = NULL;
	struct fdt_mmio_error err;
	struct fdt_mmio_audit *audit = NULL;
	static const struct fdt_mmio_allow_range allow_all[] = {
		{ 0x40000800ULL, 0x40009000ULL },
		{ 0x50000000ULL, 0x50001000ULL },
	};
	static const struct fdt_mmio_allow_range allow_gap[] = {
		{ 0x40000800ULL, 0x40007900ULL },
		{ 0x50000100ULL, 0x50001000ULL },
	};

	MMIO_CHECK(new_tree(blob, 2, 2) == 0);

	busm = fdt_add_subnode(blob, 0, "bus-m");
	MMIO_CHECK(busm >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, busm, "#address-cells", 2) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, busm, "#size-cells", 2) == 0);
	/* W0: [0x1_00000000,+0x8000) -> [0x40000000,+0x8000)
	 * W1: [0x1_00008000,+0x8000) -> [0x50000000,+0x8000) */
	p = cells;
	p = put_cells(p, 2, 0x100000000ULL);
	p = put_cells(p, 2, 0x40000000ULL);
	p = put_cells(p, 2, 0x8000);
	p = put_cells(p, 2, 0x100008000ULL);
	p = put_cells(p, 2, 0x50000000ULL);
	p = put_cells(p, 2, 0x8000);
	MMIO_CHECK(prop_cells(blob, busm, "ranges", cells,
			      (int)(p - cells)) == 0);

	busl = fdt_add_subnode(blob, busm, "bus-l");
	MMIO_CHECK(busl >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, busl, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, busl, "#size-cells", 1) == 0);
	/* V0: [0x1000,+0x1000) -> [0x1_00000000,+0x1000)
	 * V1: [0x2000,+0x1000) -> [0x1_00007800,+0x1000) */
	p = cells;
	p = put_cells(p, 1, 0x1000);
	p = put_cells(p, 2, 0x100000000ULL);
	p = put_cells(p, 1, 0x1000);
	p = put_cells(p, 1, 0x2000);
	p = put_cells(p, 2, 0x100007800ULL);
	p = put_cells(p, 1, 0x1000);
	MMIO_CHECK(prop_cells(blob, busl, "ranges", cells,
			      (int)(p - cells)) == 0);

	dev = fdt_add_subnode(blob, busl, "dev");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x1800);
	p = put_cells(p, 1, 0x1800);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);

	/* [0x1800,+0x1800) splits at V0/V1, then the V1 piece splits
	 * again at the W0/W1 boundary: three segments. */
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x1800, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(rep->n_segments == 3);
	MMIO_CHECK(rep->n_levels == 3);

	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x800, 0x40000800ULL) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 0, 0x1800, 0x2000,
				FDT_MMIO_SPACE_MEMORY, 0) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 1,
				0x100000800ULL, 0x100001000ULL,
				FDT_MMIO_SPACE_MEMORY, 0) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 2,
				0x40000800ULL, 0x40001000ULL,
				FDT_MMIO_SPACE_MEMORY,
				FDT_MMIO_RANGES_NONE) == 0);

	MMIO_CHECK(expect_seg(checks, rep, 1, 0x800, 0x800,
			      0x40007800ULL) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[1], 0, 0x2000, 0x2800,
				FDT_MMIO_SPACE_MEMORY, 1) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[1], 1,
				0x100007800ULL, 0x100008000ULL,
				FDT_MMIO_SPACE_MEMORY, 0) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[1], 2,
				0x40007800ULL, 0x40008000ULL,
				FDT_MMIO_SPACE_MEMORY,
				FDT_MMIO_RANGES_NONE) == 0);

	MMIO_CHECK(expect_seg(checks, rep, 2, 0x1000, 0x800,
			      0x50000000ULL) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[2], 0, 0x2800, 0x3000,
				FDT_MMIO_SPACE_MEMORY, 1) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[2], 1,
				0x100008000ULL, 0x100008800ULL,
				FDT_MMIO_SPACE_MEMORY, 1) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[2], 2,
				0x50000000ULL, 0x50000800ULL,
				FDT_MMIO_SPACE_MEMORY,
				FDT_MMIO_RANGES_NONE) == 0);
	MMIO_CHECK(!strcmp(rep->buses[0].path, "/bus-m/bus-l"));
	MMIO_CHECK(!strcmp(rep->buses[1].path, "/bus-m"));
	MMIO_CHECK(!strcmp(rep->buses[2].path, "/"));

	/* audit: all three segments land inside the two allowed windows */
	ret = fdt_mmio_audit(blob, dev, allow_all, 2, &audit);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(audit->verdict == FDT_MMIO_VERDICT_ALLOWED);
	MMIO_CHECK(audit->n_items == 3);
	MMIO_CHECK(audit->items[0].verdict == FDT_MMIO_VERDICT_ALLOWED);
	MMIO_CHECK(audit->items[1].verdict == FDT_MMIO_VERDICT_ALLOWED);
	MMIO_CHECK(audit->items[2].verdict == FDT_MMIO_VERDICT_ALLOWED);
	fdt_mmio_audit_free(audit);
	audit = NULL;

	/* audit: a forbidden zone between the allowed windows splits
	 * segments 1 and 2 into allowed and violating pieces */
	ret = fdt_mmio_audit(blob, dev, allow_gap, 2, &audit);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(audit->verdict == FDT_MMIO_VERDICT_VIOLATION);
	MMIO_CHECK(audit->n_items == 5);
	MMIO_CHECK(audit->items[0].verdict == FDT_MMIO_VERDICT_ALLOWED);
	MMIO_CHECK(audit->items[0].cpu_start == 0x40000800ULL);
	MMIO_CHECK(audit->items[0].cpu_end == 0x40001000ULL);
	MMIO_CHECK(audit->items[1].verdict == FDT_MMIO_VERDICT_ALLOWED);
	MMIO_CHECK(audit->items[1].cpu_start == 0x40007800ULL);
	MMIO_CHECK(audit->items[1].cpu_end == 0x40007900ULL);
	MMIO_CHECK(audit->items[1].reg_offset == 0x800);
	MMIO_CHECK(audit->items[2].verdict == FDT_MMIO_VERDICT_VIOLATION);
	MMIO_CHECK(audit->items[2].cpu_start == 0x40007900ULL);
	MMIO_CHECK(audit->items[2].cpu_end == 0x40008000ULL);
	MMIO_CHECK(audit->items[2].reg_offset == 0x900);
	MMIO_CHECK(audit->items[3].verdict == FDT_MMIO_VERDICT_VIOLATION);
	MMIO_CHECK(audit->items[3].cpu_start == 0x50000000ULL);
	MMIO_CHECK(audit->items[3].cpu_end == 0x50000100ULL);
	MMIO_CHECK(audit->items[3].reg_offset == 0x1000);
	MMIO_CHECK(audit->items[4].verdict == FDT_MMIO_VERDICT_ALLOWED);
	MMIO_CHECK(audit->items[4].cpu_start == 0x50000100ULL);
	MMIO_CHECK(audit->items[4].cpu_end == 0x50000800ULL);
	MMIO_CHECK(audit->items[4].reg_offset == 0x1100);
	/* violation items keep the per-hop ranges source */
	MMIO_CHECK(audit->items[2].n_levels == 3);
	MMIO_CHECK(audit->items[2].levels[0].ranges_index == 1);
	MMIO_CHECK(audit->items[2].levels[1].ranges_index == 0);
	MMIO_CHECK(!strcmp(audit->items[2].buses[0].path, "/bus-m/bus-l"));
	MMIO_CHECK(audit->items[3].levels[0].ranges_index == 1);
	MMIO_CHECK(audit->items[3].levels[1].ranges_index == 1);
	fdt_mmio_audit_free(audit);
	audit = NULL;

	fdt_mmio_report_free(rep);
	return 0;
}

/*
 * pci: PCI buses (3 address / 2 size cells), phys.hi space select,
 * I/O and memory windows at the same numeric offset, a PCI-to-PCI
 * bridge carrying the space class through the parent phys.hi, and
 * configuration space being unsupported.
 */
static int check_pci(unsigned *checks)
{
	char blob[BLOB_SIZE];
	fdt32_t cells[32], *p;
	int pci0, pci1, dev0, dev1, ret;
	struct fdt_mmio_report *rep = NULL;
	struct fdt_mmio_error err;
	struct fdt_mmio_audit *audit = NULL;
	static const struct fdt_mmio_allow_range allow[] = {
		{ 0x80000400ULL, 0x80000500ULL },	/* mem32 entry */
		{ 0x80000080ULL, 0x80000100ULL },	/* mem32 shadow of the
							 * I/O offset, must not
							 * legitimize the I/O bar */
	};

	MMIO_CHECK(new_tree(blob, 2, 2) == 0);

	pci0 = fdt_add_subnode(blob, 0, "pci0");
	MMIO_CHECK(pci0 >= 0);
	MMIO_CHECK(fdt_setprop_string(blob, pci0, "device_type", "pci") == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, pci0, "#address-cells", 3) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, pci0, "#size-cells", 2) == 0);
	/* W0 mem32: [0x1000,+0x2000) -> root [0x80000000,+0x2000)
	 * W1 io:    [0x1000,+0x100)  -> root [0x90000000,+0x100) */
	p = cells;
	p = put_cells(p, 1, 0x02000000);
	p = put_cells(p, 2, 0x1000);
	p = put_cells(p, 2, 0x80000000ULL);
	p = put_cells(p, 2, 0x2000);
	p = put_cells(p, 1, 0x01000000);
	p = put_cells(p, 2, 0x1000);
	p = put_cells(p, 2, 0x90000000ULL);
	p = put_cells(p, 2, 0x100);
	MMIO_CHECK(prop_cells(blob, pci0, "ranges", cells,
			      (int)(p - cells)) == 0);

	pci1 = fdt_add_subnode(blob, pci0, "pci1");
	MMIO_CHECK(pci1 >= 0);
	MMIO_CHECK(fdt_setprop_string(blob, pci1, "device_type", "pci") == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, pci1, "#address-cells", 3) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, pci1, "#size-cells", 2) == 0);
	/* W2: mem64 [0x20000,+0x1000) -> pci0 mem32 [0x1800,+0x1000) */
	p = cells;
	p = put_cells(p, 1, 0x03000000);
	p = put_cells(p, 2, 0x20000);
	p = put_cells(p, 1, 0x02000000);
	p = put_cells(p, 2, 0x1800);
	p = put_cells(p, 2, 0x1000);
	MMIO_CHECK(prop_cells(blob, pci1, "ranges", cells,
			      (int)(p - cells)) == 0);

	dev1 = fdt_add_subnode(blob, pci1, "dev1");
	MMIO_CHECK(dev1 >= 0);
	p = cells;
	p = put_cells(p, 1, 0x03000000);	/* mem64 */
	p = put_cells(p, 2, 0x20400);
	p = put_cells(p, 2, 0x100);
	MMIO_CHECK(prop_cells(blob, dev1, "reg", cells, (int)(p - cells)) == 0);

	/* note: adding dev0 shifts pci1's subtree; re-resolve below */
	dev0 = fdt_add_subnode(blob, pci0, "dev0");
	MMIO_CHECK(dev0 >= 0);
	p = cells;
	p = put_cells(p, 1, 0x02000800);	/* mem32, dev 1 */
	p = put_cells(p, 2, 0x1400);
	p = put_cells(p, 2, 0x100);
	p = put_cells(p, 1, 0x01000800);	/* I/O, dev 1 */
	p = put_cells(p, 2, 0x1080);
	p = put_cells(p, 2, 0x40);
	p = put_cells(p, 1, 0x00000800);	/* config, dev 1 */
	p = put_cells(p, 2, 0);
	p = put_cells(p, 2, 0x80);
	MMIO_CHECK(prop_cells(blob, dev0, "reg", cells, (int)(p - cells)) == 0);

	/* fdt_add_subnode() inserts before existing children, so node
	 * offsets saved across sibling creation may be stale */
	MMIO_CHECK((pci0 = fdt_path_offset(blob, "/pci0")) >= 0);
	MMIO_CHECK((pci1 = fdt_path_offset(blob, "/pci0/pci1")) >= 0);
	MMIO_CHECK((dev0 = fdt_path_offset(blob, "/pci0/dev0")) >= 0);
	MMIO_CHECK((dev1 = fdt_path_offset(blob, "/pci0/pci1/dev1")) >= 0);

	/* mem32 BAR: BDF bits are source only, not arithmetic */
	ret = fdt_mmio_translate(blob, dev0, 0, 0, 0x100, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(rep->n_segments == 1);
	MMIO_CHECK(rep->n_levels == 2);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x100, 0x80000400ULL) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 0, 0x1400, 0x1500,
				FDT_MMIO_SPACE_PCI_MEM32, 0) == 0);
	MMIO_CHECK(rep->segments[0].levels[0].pci_hi == 0x02000800);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 1,
				0x80000400ULL, 0x80000500ULL,
				FDT_MMIO_SPACE_MEMORY,
				FDT_MMIO_RANGES_NONE) == 0);
	MMIO_CHECK(rep->segments[0].levels[1].pci_hi == 0);
	MMIO_CHECK(rep->buses[0].addr_cells == 3);
	MMIO_CHECK(!strcmp(rep->buses[0].path, "/pci0"));
	MMIO_CHECK(!strcmp(rep->buses[1].path, "/"));
	fdt_mmio_report_free(rep);
	rep = NULL;

	/* I/O BAR at the same numeric offset takes the I/O window */
	ret = fdt_mmio_translate(blob, dev0, 1, 0, 0x40, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x40, 0x90000080ULL) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 0, 0x1080, 0x10c0,
				FDT_MMIO_SPACE_PCI_IO, 1) == 0);
	MMIO_CHECK(rep->segments[0].levels[0].pci_hi == 0x01000800);
	fdt_mmio_report_free(rep);
	rep = NULL;

	/* configuration space is unsupported */
	ret = fdt_mmio_translate(blob, dev0, 2, 0, 0x80, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_UNSUPPORTED);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == pci0);
	MMIO_CHECK(err.reg_offset == 0);

	/* PCI-to-PCI bridge: mem64 -> mem32 (via parent phys.hi) -> root */
	ret = fdt_mmio_translate(blob, dev1, 0, 0, 0x100, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(rep->n_segments == 1);
	MMIO_CHECK(rep->n_levels == 3);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x100, 0x80000c00ULL) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 0, 0x20400,
				0x20500, FDT_MMIO_SPACE_PCI_MEM64, 0) == 0);
	MMIO_CHECK(rep->segments[0].levels[0].pci_hi == 0x03000000);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 1, 0x1c00,
				0x1d00, FDT_MMIO_SPACE_PCI_MEM32, 0) == 0);
	MMIO_CHECK(rep->segments[0].levels[1].pci_hi == 0x02000000);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 2,
				0x80000c00ULL, 0x80000d00ULL,
				FDT_MMIO_SPACE_MEMORY,
				FDT_MMIO_RANGES_NONE) == 0);
	MMIO_CHECK(!strcmp(rep->buses[0].path, "/pci0/pci1"));
	MMIO_CHECK(!strcmp(rep->buses[1].path, "/pci0"));
	MMIO_CHECK(!strcmp(rep->buses[2].path, "/"));
	fdt_mmio_report_free(rep);
	rep = NULL;

	/* audit: mem32 BAR allowed, I/O BAR violates (its mem32 shadow
	 * being allowed does not count), config space undeterminable */
	ret = fdt_mmio_audit(blob, dev0, allow, 2, &audit);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(audit->verdict == FDT_MMIO_VERDICT_VIOLATION);
	MMIO_CHECK(audit->n_items == 3);
	MMIO_CHECK(audit->items[0].verdict == FDT_MMIO_VERDICT_ALLOWED);
	MMIO_CHECK(audit->items[0].reg_index == 0);
	MMIO_CHECK(audit->items[0].cpu_start == 0x80000400ULL);
	MMIO_CHECK(audit->items[0].cpu_end == 0x80000500ULL);
	MMIO_CHECK(audit->items[1].verdict == FDT_MMIO_VERDICT_VIOLATION);
	MMIO_CHECK(audit->items[1].reg_index == 1);
	MMIO_CHECK(audit->items[1].cpu_start == 0x90000080ULL);
	MMIO_CHECK(audit->items[1].cpu_end == 0x900000c0ULL);
	MMIO_CHECK(audit->items[1].levels[0].space == FDT_MMIO_SPACE_PCI_IO);
	MMIO_CHECK(audit->items[2].verdict == FDT_MMIO_VERDICT_UNKNOWN);
	MMIO_CHECK(audit->items[2].reg_index == 2);
	MMIO_CHECK(audit->items[2].error_code == -FDT_MMIO_ERR_UNSUPPORTED);
	MMIO_CHECK(audit->items[2].error_node == pci0);
	fdt_mmio_audit_free(audit);
	audit = NULL;

	return 0;
}

/*
 * identity: empty ranges mean identity mapping (and still check the
 * parent width), missing ranges mean no mapping; empty ranges pass a
 * PCI space through only between PCI buses.
 */
static int check_identity(unsigned *checks)
{
	char blob[BLOB_SIZE];
	fdt32_t cells[24], *p;
	int bus, pcia, pcib, dev, ret;
	struct fdt_mmio_report *rep = NULL;
	struct fdt_mmio_error err;
	static const fdt32_t empty_prop[1] = { 0 };

	MMIO_CHECK(new_tree(blob, 2, 2) == 0);

	/* empty ranges: identity */
	bus = fdt_add_subnode(blob, 0, "bus-e");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 2) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop(blob, bus, "ranges", empty_prop, 0) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-e");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 2, 0x1000);
	p = put_cells(p, 1, 0x400);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x400, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(rep->n_segments == 1);
	MMIO_CHECK(rep->n_levels == 2);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x400, 0x1000) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 0, 0x1000, 0x1400,
				FDT_MMIO_SPACE_MEMORY,
				FDT_MMIO_RANGES_NONE) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 1, 0x1000, 0x1400,
				FDT_MMIO_SPACE_MEMORY,
				FDT_MMIO_RANGES_NONE) == 0);
	MMIO_CHECK(!strcmp(rep->buses[0].path, "/bus-e"));
	fdt_mmio_report_free(rep);
	rep = NULL;

	/* PCI bus with empty ranges under a PCI bus: identity, the
	 * phys.hi space bits are carried up unchanged */
	pcia = fdt_add_subnode(blob, 0, "pci-a");
	MMIO_CHECK(pcia >= 0);
	MMIO_CHECK(fdt_setprop_string(blob, pcia, "device_type", "pci") == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, pcia, "#address-cells", 3) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, pcia, "#size-cells", 2) == 0);
	p = cells;
	p = put_cells(p, 1, 0x02000000);
	p = put_cells(p, 2, 0x1000);
	p = put_cells(p, 2, 0x80000000ULL);
	p = put_cells(p, 2, 0x2000);
	MMIO_CHECK(prop_cells(blob, pcia, "ranges", cells,
			      (int)(p - cells)) == 0);
	pcib = fdt_add_subnode(blob, pcia, "pci-b");
	MMIO_CHECK(pcib >= 0);
	MMIO_CHECK(fdt_setprop_string(blob, pcib, "device_type", "pci") == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, pcib, "#address-cells", 3) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, pcib, "#size-cells", 2) == 0);
	MMIO_CHECK(fdt_setprop(blob, pcib, "ranges", empty_prop, 0) == 0);
	dev = fdt_add_subnode(blob, pcib, "dev-b");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x02000000);
	p = put_cells(p, 2, 0x1400);
	p = put_cells(p, 2, 0x100);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x100, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(rep->n_levels == 3);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x100, 0x80000400ULL) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 0, 0x1400, 0x1500,
				FDT_MMIO_SPACE_PCI_MEM32,
				FDT_MMIO_RANGES_NONE) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 1, 0x1400, 0x1500,
				FDT_MMIO_SPACE_PCI_MEM32, 0) == 0);
	MMIO_CHECK(rep->segments[0].levels[0].pci_hi ==
		   rep->segments[0].levels[1].pci_hi);
	MMIO_CHECK(rep->segments[0].levels[1].pci_hi == 0x02000000);
	fdt_mmio_report_free(rep);
	rep = NULL;

	/* PCI bus with empty ranges under a numeric bus: the space type
	 * would be discarded, which is unsupported */
	pcib = fdt_add_subnode(blob, 0, "pci-c");
	MMIO_CHECK(pcib >= 0);
	MMIO_CHECK(fdt_setprop_string(blob, pcib, "device_type", "pci") == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, pcib, "#address-cells", 3) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, pcib, "#size-cells", 2) == 0);
	MMIO_CHECK(fdt_setprop(blob, pcib, "ranges", empty_prop, 0) == 0);
	dev = fdt_add_subnode(blob, pcib, "dev-c");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x02000000);
	p = put_cells(p, 2, 0x100);
	p = put_cells(p, 2, 0x40);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x40, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_UNSUPPORTED);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == pcib);

	/* a 32-bit root: identity must check the parent width */
	MMIO_CHECK(new_tree(blob, 1, 1) == 0);
	bus = fdt_add_subnode(blob, 0, "bus-w");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 2) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop(blob, bus, "ranges", empty_prop, 0) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-w");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 2, 0x100000000ULL);
	p = put_cells(p, 1, 0x100);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x100, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_OVERFLOW);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == bus);
	MMIO_CHECK(err.reg_offset == 0);

	/* missing ranges: no mapping, not identity */
	bus = fdt_add_subnode(blob, 0, "bus-m");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 1) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-m");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x100);
	p = put_cells(p, 1, 0x40);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x40, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_NO_MAPPING);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == bus);
	MMIO_CHECK(err.reg_offset == 0);

	return 0;
}

/*
 * gaps: holes, overlapping windows and overflows fail with the bus and
 * the first failing reg offset, and never leave a partial report;
 * audits report them as undeterminable, never as allowed.
 */
static int check_gaps(unsigned *checks)
{
	char blob[BLOB_SIZE];
	fdt32_t cells[24], *p;
	int bus, dev, dev2, ret;
	struct fdt_mmio_report *rep = NULL;
	struct fdt_mmio_error err;
	struct fdt_mmio_audit *audit = NULL;
	static const struct fdt_mmio_allow_range allow_all[] = {
		{ 0, 0xffffffffffffffffULL },
	};
	static const struct fdt_mmio_allow_range allow_dev[] = {
		{ 0x1000, 0x2000 },
	};
	static const struct fdt_mmio_allow_range allow_cpu[] = {
		{ 0x40000000ULL, 0x40001000ULL },
	};

	/* holes between windows */
	MMIO_CHECK(new_tree(blob, 2, 2) == 0);
	bus = fdt_add_subnode(blob, 0, "bus-g");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 1) == 0);
	p = cells;
	p = put_cells(p, 1, 0x1000);
	p = put_cells(p, 2, 0x40000000ULL);
	p = put_cells(p, 1, 0x1000);
	p = put_cells(p, 1, 0x3000);
	p = put_cells(p, 2, 0x50000000ULL);
	p = put_cells(p, 1, 0x1000);
	MMIO_CHECK(prop_cells(blob, bus, "ranges", cells,
			      (int)(p - cells)) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-g");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x1800);
	p = put_cells(p, 1, 0xa00);
	p = put_cells(p, 1, 0x800);
	p = put_cells(p, 1, 0x100);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	dev2 = fdt_add_subnode(blob, bus, "dev-g2");
	MMIO_CHECK(dev2 >= 0);
	p = cells;
	p = put_cells(p, 1, 0x1000);
	p = put_cells(p, 1, 0x100);
	MMIO_CHECK(prop_cells(blob, dev2, "reg", cells, (int)(p - cells)) == 0);

	/* dev-g2 was inserted before dev-g: re-resolve both */
	MMIO_CHECK((dev = fdt_path_offset(blob, "/bus-g/dev-g")) >= 0);
	MMIO_CHECK((dev2 = fdt_path_offset(blob, "/bus-g/dev-g2")) >= 0);

	/* entry 0 falls into the hole at 0x2000: reg offset 0x800 */
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0xa00, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_NO_MAPPING);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == bus);
	MMIO_CHECK(err.reg_offset == 0x800);

	/* entry 1 starts in the hole */
	ret = fdt_mmio_translate(blob, dev, 1, 0, 0x100, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_NO_MAPPING);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.reg_offset == 0);

	/* a fully covered entry translates */
	ret = fdt_mmio_translate(blob, dev2, 0, 0, 0x100, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x100, 0x40000000ULL) == 0);
	fdt_mmio_report_free(rep);
	rep = NULL;

	/* audit: unmapped entries are undeterminable, not allowed */
	ret = fdt_mmio_audit(blob, dev, allow_all, 1, &audit);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(audit->verdict == FDT_MMIO_VERDICT_UNKNOWN);
	MMIO_CHECK(audit->n_items == 2);
	MMIO_CHECK(audit->items[0].verdict == FDT_MMIO_VERDICT_UNKNOWN);
	MMIO_CHECK(audit->items[0].reg_index == 0);
	MMIO_CHECK(audit->items[0].reg_offset == 0x800);
	MMIO_CHECK(audit->items[0].error_code == -FDT_MMIO_ERR_NO_MAPPING);
	MMIO_CHECK(audit->items[0].error_node == bus);
	MMIO_CHECK(audit->items[1].verdict == FDT_MMIO_VERDICT_UNKNOWN);
	MMIO_CHECK(audit->items[1].reg_index == 1);
	fdt_mmio_audit_free(audit);
	audit = NULL;

	/* audit compares CPU addresses after the upper-level remap,
	 * not device-space addresses */
	ret = fdt_mmio_audit(blob, dev2, allow_dev, 1, &audit);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(audit->verdict == FDT_MMIO_VERDICT_VIOLATION);
	MMIO_CHECK(audit->n_items == 1);
	MMIO_CHECK(audit->items[0].cpu_start == 0x40000000ULL);
	MMIO_CHECK(audit->items[0].cpu_end == 0x40000100ULL);
	fdt_mmio_audit_free(audit);
	audit = NULL;
	ret = fdt_mmio_audit(blob, dev2, allow_cpu, 1, &audit);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(audit->verdict == FDT_MMIO_VERDICT_ALLOWED);
	MMIO_CHECK(audit->n_items == 1);
	fdt_mmio_audit_free(audit);
	audit = NULL;

	/* overlapping windows in the same space */
	MMIO_CHECK(new_tree(blob, 2, 2) == 0);
	bus = fdt_add_subnode(blob, 0, "bus-o");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 1) == 0);
	p = cells;
	p = put_cells(p, 1, 0x1000);
	p = put_cells(p, 2, 0x40000000ULL);
	p = put_cells(p, 1, 0x1000);
	p = put_cells(p, 1, 0x1800);
	p = put_cells(p, 2, 0x50000000ULL);
	p = put_cells(p, 1, 0x1000);
	MMIO_CHECK(prop_cells(blob, bus, "ranges", cells,
			      (int)(p - cells)) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-o");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x1000);
	p = put_cells(p, 1, 0x100);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x100, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_OVERLAP);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == bus);
	MMIO_CHECK(err.reg_offset == 0);
	ret = fdt_mmio_audit(blob, dev, allow_all, 1, &audit);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(audit->verdict == FDT_MMIO_VERDICT_UNKNOWN);
	MMIO_CHECK(audit->items[0].error_code == -FDT_MMIO_ERR_OVERLAP);
	fdt_mmio_audit_free(audit);
	audit = NULL;

	/* window parent end beyond the 32-bit parent space */
	MMIO_CHECK(new_tree(blob, 1, 1) == 0);
	bus = fdt_add_subnode(blob, 0, "bus-v");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 1) == 0);
	p = cells;
	p = put_cells(p, 1, 0);
	p = put_cells(p, 1, 0xf0000000);
	p = put_cells(p, 1, 0x20000000);
	MMIO_CHECK(prop_cells(blob, bus, "ranges", cells,
			      (int)(p - cells)) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-v");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0);
	p = put_cells(p, 1, 0x10);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x10, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_OVERFLOW);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == bus);
	ret = fdt_mmio_audit(blob, dev, allow_all, 1, &audit);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(audit->verdict == FDT_MMIO_VERDICT_UNKNOWN);
	MMIO_CHECK(audit->items[0].error_code == -FDT_MMIO_ERR_OVERFLOW);
	fdt_mmio_audit_free(audit);
	audit = NULL;

	/* window parent end wrapping uint64_t */
	MMIO_CHECK(new_tree(blob, 2, 2) == 0);
	bus = fdt_add_subnode(blob, 0, "bus-u");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 1) == 0);
	p = cells;
	p = put_cells(p, 1, 0);
	p = put_cells(p, 2, 0xfffffffffffff000ULL);
	p = put_cells(p, 1, 0x2000);
	MMIO_CHECK(prop_cells(blob, bus, "ranges", cells,
			      (int)(p - cells)) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-u");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0);
	p = put_cells(p, 1, 0x10);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x10, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_OVERFLOW);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == bus);

	/* reg entry whose own end wraps uint64_t, device under the root */
	MMIO_CHECK(new_tree(blob, 2, 2) == 0);
	dev = fdt_add_subnode(blob, 0, "dev-r");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 2, 0xffffffffffffff00ULL);
	p = put_cells(p, 2, 0x200);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x200, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_OVERFLOW);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == 0);

	/* 3-cell bus, reg address with nonzero high cell */
	MMIO_CHECK(new_tree(blob, 2, 2) == 0);
	bus = fdt_add_subnode(blob, 0, "bus-h");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 3) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 2) == 0);
	p = cells;
	p = put_cells(p, 3, 0x1000);
	p = put_cells(p, 2, 0x40000000ULL);
	p = put_cells(p, 2, 0x1000);
	MMIO_CHECK(prop_cells(blob, bus, "ranges", cells,
			      (int)(p - cells)) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-h");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 1);		/* nonzero cell above 64 bits */
	p = put_cells(p, 2, 0);
	p = put_cells(p, 2, 0x10);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x10, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_OVERFLOW);
	MMIO_CHECK(rep == NULL);
	MMIO_CHECK(err.node == bus);

	return 0;
}

/*
 * ownership: the report owns its path and source records and stays
 * readable after the blob is destroyed; translation does not modify
 * the tree; boundary parameters are validated.
 */
static int check_ownership(unsigned *checks)
{
	char *blob, *snapshot;
	fdt32_t cells[16], *p;
	int bus, dev, ret;
	struct fdt_mmio_report *rep = NULL;
	struct fdt_mmio_error err;
	struct fdt_mmio_audit *audit = NULL;
	static const struct fdt_mmio_allow_range allow[] = {
		{ 0x40000400ULL, 0x40000500ULL },
	};

	blob = malloc(BLOB_SIZE);
	snapshot = malloc(BLOB_SIZE);
	MMIO_CHECK(blob && snapshot);

	MMIO_CHECK(new_tree(blob, 2, 2) == 0);
	bus = fdt_add_subnode(blob, 0, "bus-o");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 1) == 0);
	p = cells;
	p = put_cells(p, 1, 0x1000);
	p = put_cells(p, 2, 0x40000000ULL);
	p = put_cells(p, 1, 0x1000);
	MMIO_CHECK(prop_cells(blob, bus, "ranges", cells,
			      (int)(p - cells)) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-o");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x1400);
	p = put_cells(p, 1, 0x80);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);

	/* translation and audit are read-only */
	memcpy(snapshot, blob, BLOB_SIZE);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x80, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(!memcmp(blob, snapshot, BLOB_SIZE));
	ret = fdt_mmio_audit(blob, dev, allow, 1, &audit);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(audit->verdict == FDT_MMIO_VERDICT_ALLOWED);
	MMIO_CHECK(!memcmp(blob, snapshot, BLOB_SIZE));

	/* destroy the blob; the report and the audit must stand on
	 * their own */
	memset(blob, 0xaa, BLOB_SIZE);
	free(blob);
	free(snapshot);

	MMIO_CHECK(rep->n_segments == 1);
	MMIO_CHECK(rep->n_levels == 2);
	MMIO_CHECK(expect_seg(checks, rep, 0, 0, 0x80, 0x40000400ULL) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 0, 0x1400, 0x1480,
				FDT_MMIO_SPACE_MEMORY, 0) == 0);
	MMIO_CHECK(expect_level(checks, &rep->segments[0], 1,
				0x40000400ULL, 0x40000480ULL,
				FDT_MMIO_SPACE_MEMORY,
				FDT_MMIO_RANGES_NONE) == 0);
	MMIO_CHECK(!strcmp(rep->buses[0].path, "/bus-o"));
	MMIO_CHECK(!strcmp(rep->buses[1].path, "/"));
	MMIO_CHECK(rep->buses[0].addr_cells == 1);
	MMIO_CHECK(rep->buses[1].addr_cells == 2);

	MMIO_CHECK(audit->n_items == 1);
	MMIO_CHECK(audit->items[0].cpu_start == 0x40000400ULL);
	MMIO_CHECK(audit->items[0].cpu_end == 0x40000480ULL);
	MMIO_CHECK(audit->items[0].n_levels == 2);
	MMIO_CHECK(!strcmp(audit->items[0].buses[0].path, "/bus-o"));
	MMIO_CHECK(audit->items[0].levels[0].ranges_index == 0);

	fdt_mmio_report_free(rep);
	rep = NULL;
	fdt_mmio_audit_free(audit);
	audit = NULL;
	fdt_mmio_report_free(NULL);
	fdt_mmio_audit_free(NULL);

	/* zero-length requests: empty report, but entry and subrange
	 * must still be valid */
	blob = malloc(BLOB_SIZE);
	MMIO_CHECK(blob != NULL);
	MMIO_CHECK(new_tree(blob, 2, 2) == 0);
	bus = fdt_add_subnode(blob, 0, "bus-z");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop(blob, bus, "ranges", cells, 0) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-z");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x1400);
	p = put_cells(p, 1, 0x80);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);

	ret = fdt_mmio_translate(blob, dev, 0, 0x40, 0, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(rep != NULL);
	MMIO_CHECK(rep->n_segments == 0);
	fdt_mmio_report_free(rep);
	rep = NULL;
	ret = fdt_mmio_translate(blob, dev, 0, 0x80, 0, &rep, &err);
	MMIO_CHECK(ret == 0);
	MMIO_CHECK(rep->n_segments == 0);
	fdt_mmio_report_free(rep);
	rep = NULL;
	ret = fdt_mmio_translate(blob, dev, 0, 0x81, 0, &rep, &err);
	MMIO_CHECK(ret == -FDT_ERR_BADVALUE);
	MMIO_CHECK(rep == NULL);
	ret = fdt_mmio_translate(blob, dev, 0, 0x40, 0x80, &rep, &err);
	MMIO_CHECK(ret == -FDT_ERR_BADVALUE);
	MMIO_CHECK(rep == NULL);

	/* bad reg indices */
	ret = fdt_mmio_translate(blob, dev, 7, 0, 0x10, &rep, &err);
	MMIO_CHECK(ret == -FDT_ERR_NOTFOUND);
	MMIO_CHECK(rep == NULL);
	ret = fdt_mmio_translate(blob, dev, -1, 0, 0x10, &rep, &err);
	MMIO_CHECK(ret == -FDT_ERR_BADVALUE);
	MMIO_CHECK(rep == NULL);
	free(blob);

	/* #size-cells 0: reg entries without a length are unsupported */
	blob = malloc(BLOB_SIZE);
	MMIO_CHECK(blob != NULL);
	MMIO_CHECK(new_tree(blob, 2, 2) == 0);
	bus = fdt_add_subnode(blob, 0, "bus-s");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 0) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-s");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x100);
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0, &rep, &err);
	MMIO_CHECK(ret == -FDT_MMIO_ERR_UNSUPPORTED);
	MMIO_CHECK(rep == NULL);
	free(blob);

	/* reg property with trailing padding is malformed, not
	 * zero-filled */
	blob = malloc(BLOB_SIZE);
	MMIO_CHECK(blob != NULL);
	MMIO_CHECK(new_tree(blob, 2, 2) == 0);
	bus = fdt_add_subnode(blob, 0, "bus-p");
	MMIO_CHECK(bus >= 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#address-cells", 1) == 0);
	MMIO_CHECK(fdt_setprop_u32(blob, bus, "#size-cells", 1) == 0);
	dev = fdt_add_subnode(blob, bus, "dev-p");
	MMIO_CHECK(dev >= 0);
	p = cells;
	p = put_cells(p, 1, 0x100);
	p = put_cells(p, 1, 0x40);
	p = put_cells(p, 1, 0);		/* stray padding cell */
	MMIO_CHECK(prop_cells(blob, dev, "reg", cells, (int)(p - cells)) == 0);
	ret = fdt_mmio_translate(blob, dev, 0, 0, 0x40, &rep, &err);
	MMIO_CHECK(ret == -FDT_ERR_BADVALUE);
	MMIO_CHECK(rep == NULL);
	free(blob);

	return 0;
}

int mmio_cases(const char *group, unsigned *checks)
{
	if (!strcmp(group, "nested"))
		return check_nested(checks);
	if (!strcmp(group, "split"))
		return check_split(checks);
	if (!strcmp(group, "pci"))
		return check_pci(checks);
	if (!strcmp(group, "identity"))
		return check_identity(checks);
	if (!strcmp(group, "gaps"))
		return check_gaps(checks);
	if (!strcmp(group, "ownership"))
		return check_ownership(checks);
	return -1;
}
