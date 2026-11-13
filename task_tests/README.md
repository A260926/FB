# Shadow numerical regression tests

This directory adds a numerical regression suite for `canvas_ity`'s shadow
rendering, alongside the legacy image-hash tests in `test/test.cpp`.  It
changes no production code and no existing test: the six `shadow_*` hash
cases and the offset-shadow baseline in `task_support/baseline.cpp` keep
working exactly as before.

## Contents

- `target.cpp` -- the test cases, registered through
  `register_task_tests(Registry&)` in four groups (`kernel`, `offscreen`,
  `paint`, `state`).  Run via `bash task_support/target.sh [--mutations]`.
- `ref_shadow.hpp` -- the independent reference model the tests compare
  against.
- `shadow_ppm.cpp` -- a standalone example that renders a deterministic
  offscreen-source-plus-nested-clip scene and writes a magnified PPM of the
  alpha channel and the library-vs-reference difference.
- this document.

## Why not just more hashes

The legacy suite renders each 256x256 scene and accepts any image within
Hamming distance 5 of a stored 32-bit hash.  That is fine as a smoke test,
but it cannot say *what* changed when it fails, and small numerical
regressions either pass silently inside the tolerance or fail with no
localization at all.  Concretely, the hash suite has these gaps:

- **No stage attribution.**  A hash mixes coverage, paint evaluation, the
  blur kernel, compositing, and quantization into one number.  A 5% cut in
  the extended-box kernel's tail weight and a missing third filter pass can
  produce similar hash distances; the hash cannot distinguish them.
- **No off-canvas source coverage.**  None of the six cases places a source
  fully outside the canvas in all four directions, so an implementation
  that drops sources outside the canvas rect before convolving (instead of
  only clipping the final composite) hashes just fine.
- **No paint-alpha modulation values.**  The cases use opaque or constant
  fills; nothing checks that gradient or pattern alpha modulates the shadow
  per-pixel, that the source alpha is sampled in pre-offset device
  coordinates, or that shadow offset/blur stay in device pixels under a
  transform.
- **No state interactions.**  save/restore of shadow parameters interleaved
  with non-shadow drawing, and shadow compositing under anything but
  `source_over`, are not exercised.
- **Coarse geometry.**  256x256 scenes with hash tolerance hide one-pixel
  boundary behaviors (e.g. the composite region's bottom fringe row) that a
  small-image exact comparison pins down.

The new suite addresses each gap with direct pixel assertions on small
deterministic scenes: every pixel's alpha is compared to an independently
computed expectation, so a failure names the scene, the pixel, and the
expected and actual values.

## The independent reference

`ref_shadow.hpp` is a from-scratch model of the documented shadow pipeline.
It shares no code with the library: it does not call `paint_pixel`,
`lines_to_runs`, or `render_shadow`, and it does not reuse the
sliding-window box-filter recursion.  For each `fill_rectangle`-style draw
it computes, in `double`:

1. **Coverage** -- the device-space quad (the rectangle under the current
   affine transform) is clipped with Sutherland-Hodgman half-planes against
   each pixel square and the area is taken by the shoelace formula.  For
   convex quads this equals the library's trapezoidal scan conversion to
   float precision.
2. **Source alpha** -- solid color, linear gradient, or pattern alpha
   evaluated at the *pre-offset* device position of each source pixel, in
   the paint's own coordinate space (the reference keeps its own transform
   and inverse).
3. **Blur** -- the extended box kernel is built literally: the 1D kernel
   (interior weight `1/(2(a+r)+1)` for `|k|<=r`, tail `a/(2(a+r)+1)` at
   `|k|=r+1`, with `s=b^2/4`, `r=floor((sqrt(4s+1)-1)/2)`,
   `a=(2r+1)(r(r+1)-s)/(2s-6(r+1)^2)`) is convolved with itself three
   times, and the resulting 2D separable kernel is applied by direct
   convolution over the `6(r+1)+1`-sized window.  This protects the
   existing three-pass extended box filter -- including its exact tail
   weights -- not an idealized continuous Gaussian.
4. **Compositing** -- the blurred alpha is multiplied by the shadow color
   and `global_alpha` (applied exactly once) and blended in linear
   premultiplied space with the current Porter-Duff operator through the
   clip mask (nested rectangular clips multiply coverages).  The composite
   region reproduces the library's run-extent bounds, including the dropped
   bottom fringe row at distance exactly `border` (a real, hash-covered
   behavior of the current implementation).
5. **Quantization** -- only at the very end are pixels unpremultiplied,
   delinearized, and truncated to sRGB8 with the same 4x4 Bayer ordered
   dither as `get_image_data`.  The linear-premultiplied and sRGB8 layers
   are never conflated.

The shadow source is clipped to the canvas padded by `2*border` (device
`-border..size+border`), never to the canvas itself: sources fully outside
the canvas still cast shadows into it, and the canvas edge only limits the
final composite.  The main (non-shadow) image is simulated by the same
model, and every scene is also re-run with shadows disabled so a wrong main
image cannot be masked by a compensating shadow error.

## Quantization error budget

Comparisons are byte-exact in structure, with small tolerances absorbing
only the unavoidable noise:

- **Ordered dither**: `get_image_data` adds a Bayer threshold in `[0,1)`
  before truncation, so a mathematically identical value can land one byte
  either way.  This is the dominant term and sets the alpha tolerance of
  **+-1 byte**.
- **Float vs. double**: the library accumulates in `float` (scan
  conversion, sliding-window blur, blending), the reference in `double`.
  Worst case here is ~1e-5 of alpha -- two orders of magnitude below the
  dither term.
- **Unpremultiplication**: dividing by small alphas amplifies noise in the
  color channels, so color is only checked where the reference alpha is
  not low (>= ~0.1), with a tolerance of **+-2 bytes per channel**.

The tolerances are deliberately not wider: a 5% reduction of the kernel
tail weight (the `tails` mutation) shifts fringe alpha by ~2-5 bytes at
blur 2/4/6, and dropping the third filter pass shifts it by ~8-12 bytes, so
+-1 on alpha still rejects both.  Widening the tolerance would start
letting the tail-weight regression through.

## What each group pins down

- **kernel** -- blur levels 2, 4, 6 (radii 0, 1, 2, all with nonzero tail
  weights) plus a zero-blur offset control.  Full-image alpha comparison,
  2D kernel separability (`A(x1,y1)*A(x2,y2) == A(x1,y2)*A(x2,y1)` on
  pure-shadow pixels), and total shadow mass when nothing is clipped away.
  The offset between body and shadow leaves pure-fringe pixels whose values
  differ between a slightly reduced tail weight and a missing filter pass.
- **offscreen** -- sources fully outside the canvas in all four directions
  whose offset and/or blur reaches back inside; a source straddling the
  edge whose off-canvas half must still bleed into the blur; a deterministic
  small scene combining an off-canvas source with two nested clips; and a
  clipped-away source that still casts into the allowed region.
- **paint** -- transparent solid color, a gradient whose alpha varies with
  position (proving the source alpha is sampled before the offset), a
  code-generated transparent pattern, device-pixel offset/blur under scale
  and rotation transforms, single application of `global_alpha`, and
  linear-premultiplied compositing of a colored shadow over a colored
  background.
- **state** -- interleaved shadowed and plain drawing with save/restore of
  shadow parameters, verified through real pixels under both `source_over`
  and `destination_out`.

## Running

```
bash task_support/target.sh --mutations
```

builds and runs the four groups against the unmodified library, then the
legacy baseline (offset-shadow assertion plus the six `shadow_*` hash
cases), then rebuilds twice with fixed mutations (kernel tail weight
scaled by 0.95; off-canvas sources discarded before convolution) and
requires the new tests to reject each with a concrete pixel assertion.

The example visualizes the offscreen-plus-nested-clip scene:

```
c++ -std=c++17 -Isrc -Itask_tests task_tests/shadow_ppm.cpp -o shadow_ppm
./shadow_ppm shadow_example.ppm
```

The left panel is the library's alpha, magnified 8x; the right panel is the
library-vs-reference difference (red = alpha x32, blue = color x8) and is
essentially black on a correct build.  Everything is C++17 with the
standard library only; all inputs are generated in code -- no browser,
font, or image dependencies.
