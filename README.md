*This project is minimally maintained.*

# Libtess2

Version 1.0.3

This is refactored version of the original libtess which comes with the GLU
reference implementation. The code is good quality polygon tesselator and
triangulator. The original code comes with rather horrible interface and its'
performance suffers from lots of small memory allocations. The main point of the
refactoring has been the interface and memory allocation scheme.

A lot of the GLU tesselator documentation applies to Libtess2 too (apart from
the API), check out The OpenGL Programming Guide (the "Red Book"), Chapter 11:
Tessellators and Quadrics.

Simple bucketed memory allocator (see Graphics Gems III for reference) was added
which speeds up the code by order of magnitude (tests showed 15 to 50 times
improvement depending on data). The API allows the user to pass his own
allocator to the library. It is possible to configure the library so that the
library runs on predefined chunk of memory.

The API was changed to loosely resemble the OpenGL vertex array API. The
processed data can be accessed via getter functions. The code is able to output
contours, polygons and connected polygons. The output of the tesselator can be
also used as input for new run. I.e. the user may first want to calculate an
union all the input contours and the triangulate them.

The code is released under SGI FREE SOFTWARE LICENSE B Version 2.0.
https://directory.fsf.org/wiki/License:SGIFreeBv2

[Premake](https://premake.github.io/docs/Using-Premake/) can be used to generate
Makefiles or project definition files in various configurations, which it puts
in a `Build/` subdirectory in the checkout. The project/directory structure is:

*   `libtess2`
    -   `Source/` contains the source files and internal headers.
    -   `Include/` contains the header files in the public API.
*   `example`
    -   `Example/` contains a GUI example app.
    -   `Bin/` contains binary assets for the example app.
    -   `Contrib/` contains dependencies for the example app.
*   There is a separate Bazel build, to support testing.
    -   `Tests/` contains the tests.
    -   `task_tests/` contains the geometric-quality checks (see below).
*   `Example/` also contains a dependency-free PPM renderer,
    `ppm_example.c`.

Mikko Mononen memon@inside.org

Geometric-quality tests
=======================

`task_tests/target.cpp` adds four groups (registered with
`register_task_tests(Registry&)`: `winding`, `connectivity`, `boundary` and
`projection`) that check the actual filled region and connectivity, not just
success status, triangle count, or total area. Run them with:

    bash task_support/target.sh              # four groups + original baseline
    bash task_support/target.sh --mutations  # also checks the seeded mutations

The original `Tests/libtess2_test.cc` only checks success status, triangle
counts, and rejection of extreme input. It cannot prove that the filled region
is correct: a result with swapped triangles, overlapping facets, or a missing
hole can still report the expected triangle count and area. The new tests
close that gap.

Input corpus and exact reference
--------------------------------

Inputs are generated from small integer lattice coordinates (`|x|,|y| <= 32`)
and every edge is horizontal, vertical, with slope +1, or with slope -1. This
keeps every segment/segment intersection on a half-integer lattice. The fixed
corpus contains crossed outlines, a self-intersecting bowtie, repeated
positive/negative outlines, partially shared edges, and a hole with a disjoint
region. One figure has winding regions of 0, 1, 2 and negative values at the
same time. All five winding rules run, and `TESS_REVERSE_CONTOURS` is part of
the combinations.

The reference in `task_tests/reference.hpp` is built directly from the
original directed segments; it never calls libtess2 predicates, mesh, sweep,
or another triangulator. y coordinates of every endpoint and every
segment/segment intersection form an event list, so no two segments cross
inside one open band. Each band is sampled at two different internal heights.
At each height x is split at every crossing of the reference segments and of
the actual output triangles, and the exact rational fill (1 or 0) is compared
with the multiplicity of the actual triangles on every interval.

`task_tests/exact_rational.hpp` uses `__int128`. With the documented lattice
bound, edge components are at most 64, intersection determinants at most
2^13, and the resulting coordinate numerators/denominators stay below 2^25;
band midpoints and triangle/segment crossings multiply at most three such
quantities, so intermediates stay far below 2^127.

Output vertices are mapped back to the original lattice: a coordinate is
accepted only when it lies within `1e-4` of the nearest half-integer. No
arbitrary snapping is performed; a vertex that does not map is a test failure.

What each group checks
----------------------

*   `winding` runs all five winding rules plus reverse-contour combinations on
    the corpus and compares the region band by band.
*   `connectivity` requests `TESS_CONNECTED_POLYGONS` with `polySize` 3 and 6.
    It checks valid vertex indices, `TESS_UNDEF` in unused slots, positive
    convex polygons, two-way neighbours that share the same reversed geometric
    edge, and that only genuine boundary edges are `TESS_UNDEF`. The emitted
    polygons are split into fan triangles and take part in the region
    comparison.
*   `boundary` requests `TESS_BOUNDARY_CONTOURS`. It checks the coverage on
    both sides of every directed geometric edge, and compares the full boundary
    crossing set, band by band, with the exact reference. Collinear splitting
    and contact-point loop splitting are allowed; the test does not try to
    trace edges back to the original segments.
*   `projection` embeds the same inputs in `z = x + 2y`, passes `(-1,-2,1)` and
    its negative, verifies the returned 3D points lie on the plane, maps back
    to the 2D reference, and swaps POSITIVE/NEGATIVE for the reversed normal.
    It then repeats with 1/8 and 8 scaling, integer translation, contour
    start rotation, and input order swap. When an object is reused with a
    different output mode, the test keeps its own copy of the earlier arrays
    instead of trusting stale pointers.

Failure messages include the shape, rule, band, height, expected and actual
coverage, the directed edge for boundary errors, a fixed seed identifier and a
deterministic `unit_square_with_hole` example.

The example `Example/ppm_example.c` renders a hole, crossed outlines and a
bowtie to a PPM image with one colour per output triangle, plus boundary
contours stroked in black. Build it with only the C compiler and the standard
library, for example:

    cc -std=c99 -O2 -IInclude Example/ppm_example.c <every .c file in Source/> -lm \
       -o ppm_example && ./ppm_example out.ppm

