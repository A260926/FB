// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)
/*
 * Example for the libfdt MMIO translation reporting API
 * (libfdt/fdt_mmio.h).
 *
 * Builds a small two-level tree in memory, translates a device's reg
 * entry up to the CPU address space, prints every segment with its
 * per-hop path, then audits the device against a caller-defined list
 * of allowed CPU address ranges.
 *
 * Build and run from the top of the dtc tree:
 *
 *	cc -Ilibfdt libfdt/fdt.c libfdt/fdt_ro.c libfdt/fdt_wip.c \
 *	   libfdt/fdt_sw.c libfdt/fdt_rw.c libfdt/fdt_strerror.c \
 *	   libfdt/fdt_empty_tree.c libfdt/fdt_addresses.c \
 *	   libfdt/fdt_overlay.c libfdt/fdt_check.c libfdt/fdt_mmio.c \
 *	   tools/mmio_example.c -o mmio-example
 *	./mmio-example
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <libfdt.h>
#include <fdt_mmio.h>

static fdt32_t *put_cells(fdt32_t *p, int n, uint64_t v)
{
	int i;

	for (i = n - 1; i >= 0; i--) {
		p[i] = cpu_to_fdt32((uint32_t)(v & 0xffffffffU));
		v >>= 32;
	}
	return p + n;
}

/* / (2/2 cells) -> bus@0 (1/1 cells) mapping child [0x1000,+0x1000)
 * to CPU [0x40000000,+0x1000); device@1400 with reg <0x1400 0x100>. */
static int build_tree(char *buf, size_t size, int *device)
{
	fdt32_t prop[8], *p;
	int bus, dev, ret;

	ret = fdt_create_empty_tree(buf, size);
	if (ret)
		return ret;
	ret = fdt_setprop_u32(buf, 0, "#address-cells", 2);
	if (ret)
		return ret;
	ret = fdt_setprop_u32(buf, 0, "#size-cells", 2);
	if (ret)
		return ret;

	bus = fdt_add_subnode(buf, 0, "bus@0");
	if (bus < 0)
		return bus;
	ret = fdt_setprop_u32(buf, bus, "#address-cells", 1);
	if (ret)
		return ret;
	ret = fdt_setprop_u32(buf, bus, "#size-cells", 1);
	if (ret)
		return ret;
	p = prop;
	p = put_cells(p, 1, 0x1000);		/* child base */
	p = put_cells(p, 2, 0x40000000ULL);	/* CPU base */
	p = put_cells(p, 1, 0x1000);		/* length */
	ret = fdt_setprop(buf, bus, "ranges", prop, sizeof(prop));
	if (ret)
		return ret;

	dev = fdt_add_subnode(buf, bus, "device@1400");
	if (dev < 0)
		return dev;
	p = prop;
	p = put_cells(p, 1, 0x1400);
	p = put_cells(p, 1, 0x100);
	ret = fdt_setprop(buf, dev, "reg", prop,
			  (int)(2 * sizeof(fdt32_t)));
	if (ret)
		return ret;

	*device = fdt_path_offset(buf, "/bus@0/device@1400");
	return *device < 0 ? *device : 0;
}

static const char *space_name(uint32_t space)
{
	switch (space) {
	case FDT_MMIO_SPACE_MEMORY:
		return "mem";
	case FDT_MMIO_SPACE_PCI_IO:
		return "pci-io";
	case FDT_MMIO_SPACE_PCI_MEM32:
		return "pci-mem32";
	case FDT_MMIO_SPACE_PCI_MEM64:
		return "pci-mem64";
	default:
		return "?";
	}
}

static int show_translation(const void *fdt, int dev)
{
	struct fdt_mmio_report *rep;
	struct fdt_mmio_error err;
	uint32_t i, j;
	int ret;

	/* translate the whole first reg entry */
	ret = fdt_mmio_translate(fdt, dev, 0, 0, 0x100, &rep, &err);
	if (ret) {
		fprintf(stderr, "translate failed: %d at node %d reg+%#" PRIx64
			"\n", ret, err.node, err.reg_offset);
		return 1;
	}

	for (i = 0; i < rep->n_segments; i++) {
		const struct fdt_mmio_segment *seg = &rep->segments[i];

		printf("segment %u: reg+%#" PRIx64 " len %#" PRIx64
		       " -> cpu %#" PRIx64 "\n", i, seg->reg_offset,
		       seg->length, seg->cpu_address);
		for (j = 0; j < rep->n_levels; j++) {
			const struct fdt_mmio_level *l = &seg->levels[j];

			printf("  %-12s [%#" PRIx64 ", %#" PRIx64 ") %-9s",
			       rep->buses[j].path, l->start, l->end,
			       space_name(l->space));
			if (l->ranges_index == FDT_MMIO_RANGES_NONE)
				printf(" (identity/root)\n");
			else
				printf(" via ranges[%d]\n", l->ranges_index);
		}
	}
	fdt_mmio_report_free(rep);
	return 0;
}

static int show_audit(const void *fdt, int dev)
{
	/* Local policy: the device may touch [0x40000400, 0x40000480).
	 * The allowed ranges come from local configuration, never from
	 * the DTB being audited. */
	static const struct fdt_mmio_allow_range allowed[] = {
		{ 0x40000400ULL, 0x40000480ULL },
	};
	static const char *const verdict_names[] = {
		[FDT_MMIO_VERDICT_ALLOWED] = "allowed",
		[FDT_MMIO_VERDICT_VIOLATION] = "violation",
		[FDT_MMIO_VERDICT_UNKNOWN] = "undeterminable",
	};
	struct fdt_mmio_audit *audit;
	uint32_t i;
	int ret;

	ret = fdt_mmio_audit(fdt, dev, allowed, 1, &audit);
	if (ret) {
		fprintf(stderr, "audit failed: %d\n", ret);
		return 1;
	}

	printf("audit verdict: %s\n", verdict_names[audit->verdict]);
	for (i = 0; i < audit->n_items; i++) {
		const struct fdt_mmio_audit_item *it = &audit->items[i];

		if (it->verdict == FDT_MMIO_VERDICT_UNKNOWN)
			printf("  reg[%d]+%#" PRIx64 ": %s (error %d)\n",
			       it->reg_index, it->reg_offset,
			       verdict_names[it->verdict], it->error_code);
		else
			printf("  reg[%d]+%#" PRIx64 " cpu [%#" PRIx64
			       ", %#" PRIx64 "): %s\n",
			       it->reg_index, it->reg_offset, it->cpu_start,
			       it->cpu_end, verdict_names[it->verdict]);
	}
	fdt_mmio_audit_free(audit);
	return 0;
}

int main(void)
{
	static char blob[4096];
	int dev, ret;

	ret = build_tree(blob, sizeof(blob), &dev);
	if (ret) {
		fprintf(stderr, "failed to build example tree: %d\n", ret);
		return 1;
	}
	if (show_translation(blob, dev))
		return 1;
	if (show_audit(blob, dev))
		return 1;
	return 0;
}
