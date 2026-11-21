# On-demand paged view-dependent refinement

`libHh/SrMeshPages.{h,cpp}` adds an on-demand paging path to the selectively refinable `SrMesh`.  The ordinary
all-resident `read_pm` / `read_srm` / `write_srm` entry points are unchanged.

## Concepts

- **Directory** (`SrPageDirectory`, encoded by `SrPageCodec::encode_directory`): the fully coarsened base mesh,
  materials, bounding box, the stable per-split parent-vertex identity, and the conservative view-admission bounds
  (frustum sphere radius, uniform/directional residual errors, backface cone).  It grows linearly with the mesh but
  contains **no** neighbor face quadruplets and **no** subdivision geometry.
- **Pages** (`SrPageCodec::encode_page`): the only place the real `SrVsplit` payloads live.  Splits are grouped by
  their original contiguous split index; the caller chooses `splits_per_page` in `1..64`.  Page ids are stable
  integers; nothing serialized contains a process pointer.
- **Runtime** (`SrPagedMesh`): starts from the directory with zero split pages resident and a positive page budget.
  The resident payload is bounded by `page_budget * splits_per_page`; there is no full `_vsplits` copy and no
  startup prefetch.

## Runtime API

```cpp
SrPageDirectory dir;
SrPageCodec::export_directory(coarsened_srmesh, splits_per_page, dir);
auto dir_bytes = SrPageCodec::encode_directory(dir);

SrPagedMesh paged;
paged.initialize(dir, /*page_budget=*/8);
paged.adapt_refinement(view_params);
for (int pageid : paged.pending_pages()) {        // sorted, deduplicated missing page ids
  SrPageError err;
  paged.supply_page(pageid, encoded_page_bytes, err);  // invalid bytes are rejected, state is untouched
}
paged.set_page_budget(larger_budget);            // budget can only grow
paged.blocked_by_budget();                       // true when every resident page is pinned
GMesh active = paged.mesh().extract_gmesh();
```

Refinement still runs the original `SrMesh` `qrefine`/`qcoarsen`, `force_vsplit`, `apply_vspl`/`apply_ecol` half-edge
machinery.  When a recursive prerequisite split is missing, its page is requested and that refinement is paused
while other ready regions continue; no split is ever half-applied and no neighbor dependency is skipped.
`supply_page()` never changes the active topology; the next `adapt_refinement()` continues from the current legal
frontier.  Both geomorphs are disabled on this path while the normal `SrMesh` keeps its geomorph behavior.

An executed split pins its page until it is legally merged; the last merge on a page releases it.  When the cache is
full, the least-recently-used **unpinned** page is evicted.  If every resident page is pinned, the legal frontier is
held and `blocked_by_budget()` reports the stall until the budget is raised.

## Wire format and validation

All multi-byte values are big-endian; floats are IEEE-754 binary32.  The exact directory and page layouts are
documented above `SrPageCodec` in `libHh/SrMeshPages.h`.  Pages carry a fixed 24-byte header (magic, version, page
id, record count, body length, FNV-1a checksum) followed by fixed 72-byte records.  A page is explicitly rejected for
a bad page id, a repeated split record, truncation/checksum failure, an out-of-range vertex/face/material reference,
or non-finite geometry; on rejection nothing is evicted and the frontier, resident set and requests are unchanged.

## Example

```
paged_refinement <input.srm> <output.m> [splits_per_page (1..64, default 4)] [page_budget (default: all pages)]
```

It exports the directory and pages from an ordinary coarsened `SrMesh`, feeds the pages in batches in response to
`pending_pages()`, runs view refinement to convergence, writes the active mesh as a `.m` file, and prints the
request / residency / active-face statistics.  Only the C++ standard library is used beyond `libHh`.
