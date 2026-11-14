# Geometry-quality task tests for libtess2

These tests prove that a tesselation covers the **right region with the right
connectivity**, not merely that it succeeds and emits the expected number of
triangles. Everything here is C/C++ plus the standard library; no GoogleTest,
image library, second triangulation library, or other dependency is used.

## Files

| File | Role |
| --- | --- |
| `target.cpp` | The four registered test groups (`winding`, `connectivity`, `boundary`, `projection`). No `main` — `../task_support/main.cpp` drives it through `register_task_tests`. |
| `geo_ref.hpp` | The independent exact reference: rationals, directed segments, event bands, crossings, winding numbers, filled intervals. |
| `geo_scenes.hpp` | Fixed small-integer scenes, libtess2 glue, half-integer grid snapping, and the event-band coverage comparator. |
| `ppm_example.cpp` | Standalone PPM renderer that colors every output triangle and overlays boundaries and the original edges, so holes, crossings and boundaries are visible. |

Run everything (it also runs the original baseline) with:

```
bash ../task_support/target.sh
bash ../task_support/target.sh --mutations   # adds the two seeded mutations
```

All scenes and all colors are fixed literals; there is no random input. The
fixed seed printed on a failure is `0x1172E55` (`kSeed`), and every failing
message names the scene's contours, the winding rule, the event band or
directed edge, and the expected versus actual coverage.

## What the old tests did not cover

`../Tests/libtess2_test.cc` checks success/failure status, the triangle count
for a handful of shapes, rejection of extreme/NaN inputs, and (in
`../task_support/baseline.c`) the **total** area of one square-with-hole.
None of those can detect a result whose total area happens to be correct while
a local piece overlaps a neighbor or leaves a gap: the errors cancel in an
aggregate area and do not change the triangle count. These new tests compare
geometry region-by-region, so any local overlap or missing patch fails.

## Inputs the exact reference applies to

The reference is exact (no floating point) and is deliberately scoped to the
input class the tests use:

* contour points are integer lattice points with `|x|, |y| <= 32`;
* every edge is horizontal, vertical, or has slope `+1` or `-1`;
* therefore every proper edge-to-edge intersection lands on a
  **half-integer** grid point (coordinates are multiples of `1/2`), and every
  event y-coordinate has denominator 2.

Under those constraints the reference builds its own rational event bands
straight from the original directed segments. Segment endpoints **and**
proper segment intersections are events; inside each open band the ordering
of the crossing segments is fixed. Horizontal crossings, winding numbers and
the filled x-intervals are all evaluated with integer/rational arithmetic.
No libtess2 predicate, mesh, sweep code, or other triangulation library is
consulted.

### Quantization and integer bounds

* Output vertices are only accepted after their coordinates are normalized
  back onto the original grid (inverse of any test scale/translation). A
  normalized coordinate may be mapped to a reference half-integer only when
  the nearest half-integer is within **`1e-4`**; anything farther away fails
  the test rather than being snapped to a wrong grid point.
* Reference fractions use `long long` numerator/denominator with every
  multiply widened to `__int128`. With input numerators bounded by 32,
  reduced event fractions have numerator at most 64 (denominator 2), and the
  two sample heights per band use denominator at most 4. A crossing
  abscissa is a short sum/products of such fractions, so unreduced numerators
  stay below roughly `1e4` and denominators below 16. `__int128` holds about
  `1.7e38`, many orders of magnitude above any intermediate value here; the
  bound exists to make overflow reasoning explicit, not because it is close.

## How each group works

### winding — region coverage for every rule

Each fixed scene is run under all five winding rules
(`ODD, NONZERO, POSITIVE, NEGATIVE, ABS_GEQ_TWO`) and with
`TESS_REVERSE_CONTOURS` both off and on. Output polygons are fanned into
triangles on the reference grid. For every event band — reference events
**plus the snapped output vertex heights** — two distinct interior heights
(1/3 and 2/3 through the band) are sampled. Each horizontal line is split at
every reference and triangle interval endpoint, and on each resulting
interval the triangle-soup coverage multiplicity must be exactly `1` where
the rule says filled and `0` outside. Overlap (multiplicity 2) and a gap (0
where 1 is expected) both fail, even when the total area would still match.

One scene (`winding_mix_0_1_2_negative`) contains regions with winding `0`,
`1`, `2` and a negative value simultaneously, and a dedicated case probes each
of those winding values and each rule's selection directly. With an
auto-computed normal the library's `CheckOrientation()` flips a uniformly
reversed input back; `sceneForRun()` reproduces that exact sign convention in
the reference instead of assuming reversal always negates winding.

The fixed scenes include crossing contours, a self-intersecting bowtie,
same- and opposite-orientation duplicate contours, a partially shared edge,
holes, and disjoint regions.

### connectivity — `TESS_CONNECTED_POLYGONS`

Run with both `polySize = 3` and `polySize = 6`. For every output polygon the
tests verify that vertex indices are in range, unused vertex and neighbour
slots are `TESS_UNDEF`, the ring has positive area and is convex (collinear
merged vertices allowed), and the fanned triangles still satisfy the full
band coverage comparison. For every edge:

* a `TESS_UNDEF` neighbour is allowed only where the two sides genuinely
  differ in fill status (a true boundary);
* a concrete neighbour must be in range, must not be the polygon itself, and
  must sit on an edge that is filled on both sides;
* the neighbour must own the **exact reversed geometric edge**, and that
  neighbour's corresponding slot must point back, so the link is both mutual
  and geometrically coincident — not merely two indices that reference each
  other over different edges.

### boundary — `TESS_BOUNDARY_CONTOURS`

The reference marks every half-unit boundary atom along the original
segments, subdivided at all event endpoints, wherever the fill status on the
two sides differs. The tesselator's boundary edges are split into the same
half-unit atoms (collinear subdivisions and contact-point ring splits are
allowed) and the two geometric sets must be identical — the whole boundary
interval, not just a contour count. Each output edge is additionally checked
to separate a filled from an unfilled side, and each closed ring is checked
to keep the fill on one consistent side with nonzero area.

### projection — the `(x, y, x+2y)` plane

The same 2D scenes are embedded as `(x, y, x+2y)` and tesselated with the
explicit normal `(-1,-2,1)` and its reverse `(1,2,-1)`. Every returned 3D
vertex is checked to remain on the plane, then the 2D coordinates are mapped
back and compared against the same exact band reference. Reversing the normal
must swap the `POSITIVE`/`NEGATIVE` selection while leaving the geometry
otherwise identical. Each scene is also re-run at scales `1/8` and `8`, with
an integer translation, with a contour start rotated, and with the contour
input order swapped. A separate case reuses one tesselator object across two
output modes and keeps its **own deep copies** of the first run's arrays
before the second run can invalidate them.

## PPM example

`ppm_example.cpp` is a standalone renderer (not part of the test binary). It
builds one fixed scene containing a square with a hole, a diamond crossing a
rectangle, and a disjoint square, then writes a binary PPM (`P6`). Every
output triangle is filled with its own deterministic color, the library's
boundary contours are drawn in black, and the original input edges in dark
blue, so the hole (background showing through), the crossing and the
boundaries are all visible. Build and run it with only the standard library:

```
c++ -std=c++17 -O1 -IInclude task_tests/ppm_example.cpp build/*.o -lm -o ppm_example
./ppm_example sample.ppm
```

The pixel checks used while developing it confirm the hole center stays on
the background, filled rims and the disjoint square are painted, many
distinct per-triangle colors appear, and both black boundary and blue input
pixels are present.
