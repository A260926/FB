/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */
#ifndef FDT_MMIO_H
#define FDT_MMIO_H
/*
 * libfdt - Flat Device Tree manipulation
 *
 * MMIO address translation reporting: resolve a device's reg entry
 * subrange through the chain of bus ranges up to the root (CPU)
 * address space, producing an allocated, self-contained report.
 *
 * This interface is read-only: it never modifies the blob, never
 * generates DTS, never touches hardware and uses no kernel services.
 * The report owns all of its data (including the bus path strings),
 * so it remains valid after the blob has been freed or modified.
 */

#include <libfdt_env.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Translation-specific error codes.  Functions in this header return
 * 0 on success or a negative error code on failure.  Codes are either
 * the libfdt FDT_ERR_* codes (for bad blobs, bad parameters, missing
 * properties, ...) or the FDT_MMIO_ERR_* codes below for translation
 * failures.  The values continue after FDT_ERR_MAX so callers can
 * switch on both ranges without ambiguity.
 */
#define FDT_MMIO_ERR_NO_MAPPING	20
	/* FDT_MMIO_ERR_NO_MAPPING: the requested range is not mapped on
	 * this path: a bus on the way to the root has no "ranges"
	 * property at all, or the "ranges" windows leave a hole over
	 * the requested range. */
#define FDT_MMIO_ERR_OVERLAP	21
	/* FDT_MMIO_ERR_OVERLAP: a traversed "ranges" property contains
	 * overlapping child windows in the same address space, so the
	 * mapping is ambiguous. */
#define FDT_MMIO_ERR_OVERFLOW	22
	/* FDT_MMIO_ERR_OVERFLOW: an address or the end of a half-open
	 * interval cannot be represented: more than 64 significant
	 * bits, a uint64_t wrap, or a value that does not fit the
	 * address width of the target bus. */
#define FDT_MMIO_ERR_UNSUPPORTED 23
	/* FDT_MMIO_ERR_UNSUPPORTED: the request is outside the
	 * supported boundary: PCI configuration space, a reg entry
	 * without a length (#size-cells == 0), a PCI bus that does not
	 * use 3 address and 2 size cells, or an identity mapping that
	 * would have to discard a PCI space type. */

/* Address space class of one level of a translated segment. */
enum fdt_mmio_space {
	FDT_MMIO_SPACE_MEMORY = 0,	/* plain numeric (memory) space */
	FDT_MMIO_SPACE_PCI_IO = 1,	/* PCI I/O space */
	FDT_MMIO_SPACE_PCI_MEM32 = 2,	/* PCI 32-bit memory space */
	FDT_MMIO_SPACE_PCI_MEM64 = 3,	/* PCI 64-bit memory space */
};

#define FDT_MMIO_RANGES_NONE	(-1)
	/* ranges_index value used when the hop out of a level was an
	 * identity mapping (empty "ranges"), and for the final (root)
	 * level which has no outgoing hop. */

/* One bus on the path from the device's bus to the root. */
struct fdt_mmio_bus {
	int node;	/* node offset; only meaningful while the blob lives */
	char *path;	/* full node path, owned by the report/audit */
	uint32_t addr_cells;	/* #address-cells of this bus */
};

/* The span of one segment at one level of the translation path. */
struct fdt_mmio_level {
	uint64_t start;		/* span start in this level's address space */
	uint64_t end;		/* half-open span end */
	uint32_t space;		/* enum fdt_mmio_space */
	uint32_t pci_hi;	/* raw PCI phys.hi source bits (BDF, BAR,
				 * prefetch, ...); 0 for numeric spaces.
				 * Informational only, never used in
				 * address arithmetic. */
	int ranges_index;	/* index of the "ranges" entry of this
				 * level's bus used to reach the next
				 * level, or FDT_MMIO_RANGES_NONE */
};

/* One contiguous piece of the translated request. */
struct fdt_mmio_segment {
	uint64_t reg_offset;	/* byte offset within the reg entry's range */
	uint64_t length;	/* bytes covered by this segment */
	uint64_t cpu_address;	/* start address in the root (CPU) space */
	struct fdt_mmio_level *levels;	/* n_levels entries, owned by the
					 * report; levels[0] is the device's
					 * bus, levels[n_levels-1] the root */
};

/*
 * The translation report.  Everything is owned by the report and is
 * released by fdt_mmio_report_free().  Segments are sorted by
 * reg_offset.  All segments share the same bus path (report->buses),
 * level i of every segment lies in the address space of buses[i].
 */
struct fdt_mmio_report {
	uint32_t n_segments;
	uint32_t n_levels;	/* path depth: number of buses/levels */
	struct fdt_mmio_segment *segments;	/* n_segments entries */
	struct fdt_mmio_bus *buses;		/* n_levels entries */
};

/* Detailed failure information for fdt_mmio_translate(). */
struct fdt_mmio_error {
	int code;		/* the (negative) error code returned */
	int node;		/* bus node where translation failed:
				 * the node whose "ranges" (missing, empty
				 * or windowed) was being applied, or the
				 * device's parent bus for reg entry
				 * errors; -1 if not applicable */
	uint64_t reg_offset;	/* offset within the reg entry's range of
				 * the first byte that could not be
				 * translated */
};

/**
 * fdt_mmio_translate - translate a reg entry subrange to CPU addresses
 * @fdt:		pointer to the device tree blob (read-only)
 * @nodeoffset:	offset of the device node
 * @reg_index:	index of the reg entry to translate
 * @offset:	byte offset of the subrange within the reg entry's range
 * @length:	length of the subrange in bytes (may be 0)
 * @reportp:	where to store the allocated report (set to NULL on
 *		failure; a zero-length request yields an empty report)
 * @error:	optional, filled with failure details when the call
 *		fails; may be NULL
 *
 * The reg entry is decoded with the device parent's #address-cells and
 * #size-cells (1-4 cells each; #size-cells 0 is unsupported).  The
 * subrange is then walked up to the root bus by bus: every hop must
 * cover the whole subrange, splitting it at "ranges" window
 * boundaries.  A missing "ranges" property means the path is unmapped,
 * an empty one means identity mapping; the two are not interchangeable.
 *
 * Buses are plain numeric buses, or PCI buses (device_type == "pci",
 * 3 address and 2 size cells) where phys.hi bits 25:24 select the
 * space (I/O, 32-bit or 64-bit memory; configuration space is
 * unsupported) and only the lower two cells take part in arithmetic.
 *
 * On failure no report is produced (never a partial one) and, if
 * @error is non-NULL, it describes the bus and the first reg offset
 * that failed.
 *
 * Returns 0 on success, or a negative FDT_ERR_ or FDT_MMIO_ERR_ code.
 */
int fdt_mmio_translate(const void *fdt, int nodeoffset, int reg_index,
		       uint64_t offset, uint64_t length,
		       struct fdt_mmio_report **reportp,
		       struct fdt_mmio_error *error);

/**
 * fdt_mmio_report_free - release a translation report
 * @report:	report returned by fdt_mmio_translate(), may be NULL
 */
void fdt_mmio_report_free(struct fdt_mmio_report *report);

/**
 * fdt_mmio_reg_entry - read one reg entry's device-space base and size
 * @fdt:		pointer to the device tree blob (read-only)
 * @nodeoffset:	offset of the device node
 * @reg_index:	index of the reg entry
 * @addressp:	optional, device-space base address of the entry
 *		(for PCI: the numeric phys.mid:phys.lo part)
 * @sizep:	optional, size of the entry in bytes
 *
 * Returns 0 on success, -FDT_ERR_NOTFOUND when the reg property or the
 * entry does not exist, or another negative error code.
 */
int fdt_mmio_reg_entry(const void *fdt, int nodeoffset, int reg_index,
		       uint64_t *addressp, uint64_t *sizep);

/* A caller-supplied allowed CPU address interval (half-open). */
struct fdt_mmio_allow_range {
	uint64_t start;	/* first allowed CPU physical address */
	uint64_t end;	/* half-open end */
};

enum fdt_mmio_verdict {
	FDT_MMIO_VERDICT_ALLOWED = 0,	/* fully inside the allowed set */
	FDT_MMIO_VERDICT_VIOLATION = 1,	/* touches CPU addresses outside
					 * the allowed set */
	FDT_MMIO_VERDICT_UNKNOWN = 2,	/* cannot be determined: the
					 * mapping has a hole, overlap,
					 * overflow or unsupported space */
};

/* One audited piece of a device's reg space. */
struct fdt_mmio_audit_item {
	uint32_t verdict;	/* enum fdt_mmio_verdict */
	int reg_index;		/* reg entry the piece belongs to */
	uint64_t reg_offset;	/* offset within the reg entry's range */
	uint64_t cpu_start;	/* CPU half-open interval of the piece */
	uint64_t cpu_end;	/* (both 0 for VERDICT_UNKNOWN) */
	int error_code;		/* translation error for VERDICT_UNKNOWN,
				 * 0 otherwise */
	int error_node;		/* failing bus node for VERDICT_UNKNOWN,
				 * -1 otherwise */
	uint32_t n_levels;
	struct fdt_mmio_level *levels;	/* per-hop spans and ranges source;
					 * NULL for VERDICT_UNKNOWN */
	struct fdt_mmio_bus *buses;	/* bus path for levels; NULL for
					 * VERDICT_UNKNOWN */
};

/*
 * Audit result.  The overall verdict is FDT_MMIO_VERDICT_VIOLATION if
 * any item is a violation, else FDT_MMIO_VERDICT_UNKNOWN if any item
 * is undeterminable, else FDT_MMIO_VERDICT_ALLOWED.  Items cover every
 * reg entry of the device; everything is owned by the audit and
 * released by fdt_mmio_audit_free().
 */
struct fdt_mmio_audit {
	uint32_t verdict;	/* enum fdt_mmio_verdict */
	uint32_t n_items;
	struct fdt_mmio_audit_item *items;
};

/**
 * fdt_mmio_audit - check a device's reg space against allowed ranges
 * @fdt:		pointer to the device tree blob (read-only)
 * @nodeoffset:	offset of the device node
 * @allowed:	caller-supplied allowed CPU address intervals
 * @n_allowed:	number of entries in @allowed (0 allows nothing)
 * @auditp:	where to store the allocated audit
 *
 * Every reg entry of the device is translated to the root address
 * space with fdt_mmio_translate() and each resulting physical fragment
 * is compared against @allowed.  Fragments outside every allowed
 * interval are reported as violations with their reg offset, CPU
 * half-open interval and per-hop ranges source.  Entries that cannot
 * be translated (holes, overlaps, overflows, unsupported spaces) are
 * reported as undeterminable and are never treated as allowed.
 *
 * The allowed intervals come from the caller's own local
 * configuration; they are never derived from the audited DTB.  The
 * verdict only expresses the given address policy: it is not evidence
 * that any hardware isolation is in effect.
 *
 * Returns 0 on success (the audit is reported through @auditp even
 * when violations are found), or a negative error code for bad
 * parameters or an unusable blob.
 */
int fdt_mmio_audit(const void *fdt, int nodeoffset,
		   const struct fdt_mmio_allow_range *allowed,
		   uint32_t n_allowed,
		   struct fdt_mmio_audit **auditp);

/**
 * fdt_mmio_audit_free - release an audit
 * @audit:	audit returned by fdt_mmio_audit(), may be NULL
 */
void fdt_mmio_audit_free(struct fdt_mmio_audit *audit);

#ifdef __cplusplus
}
#endif

#endif /* FDT_MMIO_H */
