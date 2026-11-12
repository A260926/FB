# Grayscale reconstruction contract tests

`checks/gray_contract.c` + `checks/gray_ref.h` add an independent
numerical regression for Leptonica's grayscale reconstruction chain:

* `pixSeedfillGray()` (Vincent's hybrid scan+FIFO method)
* `pixSeedfillGraySimple()` (iterative scan method)
* `pixHDome()`
* `pixSeedfillGrayBasin()`

The formal algorithms and the public API are unchanged; this is a
test-only addition.  Everything is generated in C — no external
images, no image codecs.

## Why a new test (coverage gaps in the old regression)

The pre-existing grayfill checks only compare the hybrid and the
simple implementations against *each other* on smooth surfaces
(`checks/old_numeric.c`: a smooth `20 + |(100-x)(100-y)|/50` surface
and its inverse, mask values stay inside 20..235; `checks/baseline.c`:
a flat mask of 200 with one seed of 100).  Two implementations that
share the same mathematical idea can agree with each other and still
be wrong in the same way, and on those inputs:

* the fill never has to squeeze through a narrow (single-pixel)
  channel or turn several corners, so the FIFO propagation phase
  after the raster/anti-raster scans is barely exercised;
* the mask never reaches 255, so a clip of `mask == 255` down to 254
  is invisible;
* the propagation never needs a downward step at the left image
  border, so a lost `j == 0` FIFO move is invisible;
* `pixHDome()` and `pixSeedfillGrayBasin()` are not covered at all.

The two seeded degenerations in `checks/mutants/` (a lowered 255
plateau, and a 4-neighbor FIFO that drops downward propagation at the
left border) therefore still pass `checks/baseline.sh` and the old
numeric comparison, while the contract cases here reject them with
`GRAY_MISMATCH`.

## Independence of the reference (`checks/gray_ref.h`)

Expected values come from the threshold-set *definition* of grayscale
reconstruction, not from the library: for every nonzero level `t`,
the connected components of `{p : mask(p) >= t}` are found, and a
component belongs to the result's level set at `t` iff it contains a
pixel with `seed >= t`; the output gray value is the combination of
the level sets (their maximum, since they are nested).

The reference module:

* calls no Leptonica function — no seedfill, no morphology, no
  connected-component finder, no priority flood;
* uses only plain C arrays and its own breadth-first graph traversal;
  it does not copy the library's raster/anti-raster scan pair or its
  FIFO propagation structure;
* computes 4- and 8-connectivity independently from the definition;
  neither result is derived from the other;
* is a header-only module so the fixed build line in
  `checks/target.sh` needs no changes.

The same reference is reused for the derived operations, from their
documented constructions, never from library-produced intermediates:
h-dome is `mask - R_mask(mask - height)` (saturating subtraction),
and the basin fill is the binary-seed / saturating `mask + delta` /
invert-reconstruct-reinvert composition.

`pixSeedfillGrayInv()` is deliberately **not** treated as the
mathematical dual of `pixSeedfillGray()`: it has its own low-level
routine with different boundary semantics, so no contract here is
derived for it by inversion duality.  The basin operation is covered
instead through its explicitly documented invert/reconstruct/invert
recipe.

## What is covered

Group `thresholds` (core reconstruction, both implementations, both
connectivities, per-pixel against the reference):

* single-pixel-wide mazes with multiple turns that force propagation
  *after* the two scans; the last turn lies on the left image border
  in one case and on the right border in its mirror;
* a diagonal-only passage (the unique connection exists for
  8-connectivity only — the two connectivities must disagree);
* several seeds of different heights on one wide platform;
* sombrero rims with saddles at different heights (the leak level is
  set by the highest saddle);
* a zero isolation band, and values around 1, 254, 255;
* 255 plateaus with 255 seeds (must reconstruct to exactly 255);
* rotation/mirror/transpose invariance (transform, fill, map back);
* monotonicity when the legal seed or the mask is raised, and
  idempotence of a second reconstruction, both checked together with
  the per-pixel reference;
* fixed-seed random images with an explicit small size cap
  (`GRAY_RAND_MAX_DIM = 33`).

Group `storage`:

* widths 1, 3, 5, 31, 33 (real PIX rows carry 1..3 padding bytes);
* the invalid tail bytes of seed and mask are filled with *different*
  patterns; they must not influence the valid region, the seed
  padding must be left alone, and the mask (valid bytes and padding)
  must be byte-identical after the fill;
* degenerate geometries (1x1, 1xN, Nx1, wide-short) with dense
  extreme values.

Group `derived`:

* `pixHDome`: disconnected peaks of different heights, `height` 0,
  1, normal, 255 and beyond-range (saturating ends), and a
  diagonal-only leak that makes 4- and 8-connectivity differ;
* `pixSeedfillGrayBasin`: several disconnected basins (one unseeded),
  `delta` 0 (exact copy), 1, normal and 255, saturation of
  `mask + delta` at 255 (which makes the inverted seed vanish), an
  empty seed image, and a diagonal-only spill that makes the
  connectivities differ;
* both derived operations must leave the caller's inputs untouched.

## Key counterexamples

1. **255 plateau.**  Mask all 255, one 255 seed.  An implementation
   that clips `mask == 255` to 254 in its scan update returns 254
   everywhere; the reference says 255.
2. **Left-border propagation.**  A 6x5 single-pixel maze whose last
   leg can only be reached by a downward FIFO step at column 0 after
   the scan pair.  Dropping that step leaves the bottom rows at 0.
3. **Diagonal passage.**  Two plateaus touching at one diagonal
   corner: 8-connectivity floods both, 4-connectivity does not —
   computed independently, not one from the other.

## Scope of the selected old tests

`checks/target.sh` still runs the old numeric checks on the clean
library: `checks/baseline.c` (flat 200 mask, single 100 seed — a
convex fill completed by the scans alone) and `checks/old_numeric.c`
(hybrid vs simple agreement on smooth surfaces, mask values 20..235).
They are smoke/numeric-agreement checks; they carry no absolute
ground truth, never reach 255, never need left-border FIFO
propagation, and do not touch the derived operations — which is why
the two mutants still pass them.

## Running

```
bash checks/target.sh
```

builds the library, compiles and runs all contract cases
(`thresholds storage derived`), runs the old numeric checks, then
links the same cases against the two seeded mutants in
`checks/mutants/` and requires them to fail with `GRAY_MISMATCH`.
On any numeric deviation the failing case prints `GRAY_MISMATCH`
with case/function/connectivity, the first bad coordinate and the
expected/actual values, followed by a `GRAY_REPLAY` dump of the raw
input arrays (row-major C initializers) that can be pasted into a
reproducer directly.
