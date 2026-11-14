# MMIO address translation reporting (fdt_mmio.h)

`libfdt/fdt_mmio.h` resolves a device's `reg` entry (or a byte
subrange of one) through the chain of bus `ranges` properties up to
the root (CPU) address space and returns an allocated, self-contained
report of the segmented mapping.  It is strictly read-only: it never
modifies the blob, never generates DTS, never accesses hardware and
uses no kernel interfaces.  The report owns all of its data —
including the bus path strings — so it stays valid after the blob has
been freed or modified.  Nothing is fabricated: padding or missing
windows are never silently treated as address zero.

```c
int fdt_mmio_translate(const void *fdt, int nodeoffset, int reg_index,
		       uint64_t offset, uint64_t length,
		       struct fdt_mmio_report **reportp,
		       struct fdt_mmio_error *error);
void fdt_mmio_report_free(struct fdt_mmio_report *report);
int fdt_mmio_reg_entry(const void *fdt, int nodeoffset, int reg_index,
		       uint64_t *addressp, uint64_t *sizep);
int fdt_mmio_audit(const void *fdt, int nodeoffset,
		   const struct fdt_mmio_allow_range *allowed,
		   uint32_t n_allowed,
		   struct fdt_mmio_audit **auditp);
void fdt_mmio_audit_free(struct fdt_mmio_audit *audit);
```

## Supported boundary

### Numeric buses

* `#address-cells` and `#size-cells` come from the device's parent bus
  node, with the usual libfdt defaults (2 and 1) when absent.
* Addresses and lengths of 1 to 4 cells are supported.  Cells beyond
  the low 64 bits must be zero; every address and every half-open
  interval end must be representable in `uint64_t`.
* All cells are interpreted big-endian, regardless of host width or
  endianness.
* `reg` entries without a length (`#size-cells == 0`) are explicitly
  unsupported.
* A `reg` property whose length is not a multiple of the entry size
  (trailing padding) is malformed, not zero-filled.

### PCI buses (device_type = "pci")

* A PCI bus node must use exactly 3 address cells and 2 size cells.
* The high address cell (`phys.hi`) carries the space and attribute
  bits; it is not a 96-bit numeric address.  Bits 25:24 select the
  space: I/O, 32-bit memory or 64-bit memory.  Configuration space is
  unsupported for now.
* Window matching compares only the space class and the numeric
  interval formed by the lower two cells (`phys.mid:phys.lo`).  BDF,
  BAR and prefetch bits are preserved in the report as source
  (`pci_hi`) but never take part in address arithmetic.
* When the parent bus is also PCI, the space class of the next hop is
  taken from the selected window's parent `phys.hi`.  When the parent
  bus is numeric, the address becomes a plain numeric address.
* I/O and memory windows at the same numeric offset coexist and are
  interpreted independently.

### ranges semantics

* A missing `ranges` property means the path is not mapped; an empty
  `ranges` property means identity mapping.  The two are never
  interchangeable.
* Identity mappings still check that the parent bus address width can
  hold the value, and can only pass a PCI space through between two
  PCI buses (the space type must not be discarded).
* In a `ranges` entry, the child address uses this bus's
  `#address-cells`, the parent address the parent bus's, and the
  length this bus's `#size-cells`.
* Zero-length windows cover nothing and are ignored.  Windows whose
  child space is PCI configuration space are unusable and ignored.
* Overlapping child windows in the same space make the mapping
  ambiguous and are an error, whether or not the request touches the
  overlap.
* Every hop must cover the whole request, not just the first address.
  A subrange spanning several adjacent windows is split and continues
  at the next level, where windows may split the pieces again.

### Errors

`fdt_mmio_translate()` returns 0 or a negative error code and never
leaves a partial report behind.  In addition to the usual `FDT_ERR_*`
codes (bad blob, bad parameters, missing properties, malformed
property lengths), translation failures use:

* `-FDT_MMIO_ERR_NO_MAPPING` — a bus on the path has no `ranges`, or
  the windows leave a hole over the request.
* `-FDT_MMIO_ERR_OVERLAP` — overlapping windows in one space.
* `-FDT_MMIO_ERR_OVERFLOW` — an address or half-open interval end is
  not representable (more than 64 significant bits, a `uint64_t`
  wrap, or a value that does not fit the target bus width).
* `-FDT_MMIO_ERR_UNSUPPORTED` — PCI configuration space, a `reg`
  entry without a length, a PCI bus with wrong cell counts, or an
  identity mapping that would discard a PCI space type.

When the call fails and a `struct fdt_mmio_error` is given, it names
the bus node where translation failed (the node whose `ranges` —
missing, empty or windowed — was being applied, or the device's
parent bus for `reg` entry errors) and the offset within the `reg`
entry of the first byte that could not be translated.

A zero-length request returns an empty report, but the `reg` entry
and the subrange itself must still be valid.

### The report

Segments are sorted by offset within the original `reg` entry.  Each
segment records, for every level from the device's bus up to the
root, the address span, the space class, the raw PCI `phys.hi` source
bits, and the index of the `ranges` entry used to climb to the next
level (`FDT_MMIO_RANGES_NONE` for identity hops and the final level).
`report->buses[]` gives the shared bus path with full node paths and
`#address-cells`.  Node offsets are only meaningful while the blob
lives; the path strings are owned copies.

## Auditing

`fdt_mmio_audit()` translates every `reg` entry of a device with
`fdt_mmio_translate()` and compares each resulting physical fragment
against a caller-supplied list of allowed CPU address intervals:

* `FDT_MMIO_VERDICT_ALLOWED` — the fragment is fully covered by the
  allowed set.
* `FDT_MMIO_VERDICT_VIOLATION` — the fragment touches CPU addresses
  outside the allowed set.  The item reports the offset within the
  original `reg` entry, the CPU half-open interval of the offending
  piece, and the per-hop `ranges` source.
* `FDT_MMIO_VERDICT_UNKNOWN` — the entry could not be translated
  (hole, overlap, overflow or unsupported space).  Undeterminable
  entries are never treated as allowed.

The overall verdict is `VIOLATION` if any item violates, else
`UNKNOWN` if any item is undeterminable, else `ALLOWED`.

The allowed intervals must come from a separate local configuration;
they are never derived from the DTB under audit.  The audit only
evaluates the given address policy: a DTB that declares a window is
**not** evidence that any hardware isolation is actually in effect.

## Example

`tools/mmio_example.c` builds a small tree, prints the translated
segments with their per-hop path, and audits a device against a local
allowed list:

```sh
cc -Ilibfdt libfdt/*.c tools/mmio_example.c -o mmio-example
./mmio-example
```

The test suite in `tests/mmio_cases.c` (driven by
`tools/run-mmio.sh`) covers nested buses with different cell widths,
window-crossing splits, PCI bridging, identity mappings, holes,
overlaps, overflows and report ownership.
