# Bilinear emissive-map area light

This directory holds the task harness for the `BilinearAreaLight` feature. This
note documents how to build, run, and test it, what the light supports, and the
measure/conventions used for its integral and sampling distribution.

## Building and running

Everything builds offline; the pinned dependency sources are already in the
tree, so no network access is needed.

```sh
# Build the renderer (build/task/pbrt) and the unit-test binary.
bash task_support/install.sh

# Run the target tests (EmissiveMapGeometry / -Density / -Transport) and then
# the legacy baseline (triangle, distribution and constant area-light tests).
bash task_support/run.sh target

# Just the legacy baseline:
bash task_support/run.sh baseline
```

`task_support/run.sh target` also requires the new groups to execute real
assertions: each case prints `TASK_CASE_DONE <Group>.<Case> checks=<n>` and the
runner rejects any case that ran zero `TASK_CHECK` assertions.

Render the example scene from the repository root:

```sh
./build/task/pbrt scenes/emissive-map.pbrt     # writes emissive-map.exr
```

`scenes/emissive-map.pbrt` writes all triangle vertices and all emissive map
values inline, and shows an uneven emissive triangle lighting a diffuse
receiver (visible as a non-uniform pool of light).

## Using it from a scene

`AreaLightSource "bilinear"` is a new area light. The existing
`AreaLightSource "area"` and `"diffuse"` (and `DiffuseAreaLight`) are
unchanged, as is their default constant emission.

```pbrt
AttributeBegin
    AreaLightSource "bilinear"
        "integer nu" [4] "integer nv" [3]
        "rgb map" [ ...nu*nv*3 floats... ]
        "spectrum L" [1]          # optional, default 1
        "spectrum scale" [1]      # optional, default 1
        "bool twosided" [false]   # optional
        "integer samples" [16]    # optional
    Shape "trianglemesh"
        "point P" [ ...3 vertices... ]
        "float uv" [ ...3 uvs... ]   # optional; default UVs (0,0),(1,0),(1,1)
        "integer indices" [ 0 1 2 ]
AttributeEnd
```

Parameters:

- `nu`, `nv` — integers, both at least 2.
- `map` — exactly `nu*nv` linear-RGB spectrum entries, indexed as
  `map[j*nu + i]`. Grid point `(i,j)` is defined at texture coordinate
  `(i/(nu-1), j/(nv-1))`, and adjacent grid points are interpolated
  bilinearly. There is no sRGB decoding, no wrap/repeat addressing, and no
  mip filtering: evaluation is a plain bilinear lookup in the supplied values.
- `L` (default 1) and `scale` (default 1) — the final emitted radiance is
  `E(u,v) = L * scale * map(u,v)`.
- `twosided`, `samples` — behave as for `DiffuseAreaLight`.

All map colours and `L*scale` must be finite and non-negative, `nu*nv` must not
overflow an `int`, the UV triangle must be non-degenerate with all three
vertices inside `[0,1]^2`, and the shape must be a single static triangle
without alpha masking. If any of these fails, `CreateBilinearAreaLight`
reports an `Error` and returns `nullptr`; it never silently falls back to a
constant emitter.

## Supported geometry and scope

- One static, non-degenerate `Triangle` (including the default texture
  coordinates used when no `uv`/`st` is supplied).
- Any orientation, non-uniform scale, shear, and reflection. Emission uses the
  triangle's actual world-space vertices and UVs. The emitting normal is the
  unit geometric normal formed from the world vertices, flipped by
  `reverseOrientation XOR transformSwapsHandedness`. Interpolated (smooth)
  vertex normals are never used to change the emitting hemisphere.
- The triangle's UV mapping must be non-degenerate (the UV triangle has
  nonzero area); the UVs may otherwise form an arbitrary oblique triangle
  inside `[0,1]^2`.
- Not supported: non-triangle shapes, alpha-masked triangles, animated
  transforms, and multiple triangles sharing one light.

## Measure conventions for the integral and the sampling distribution

Positions are sampled on the triangle with **area measure**. The target density
is

    pA(x) = Y(E(x)) / Z,     Z = ∫_triangle Y(E(x)) dA,

where `Y` is `Spectrum::y()` (luminance). `Power()` returns
`π * ∫_triangle E(x) dA` (multiplied by 2 when `twosided`). The integral and
the sampling distribution cover exactly the part of the texture square
actually covered by the UV triangle — not the whole map rectangle. Integrals
across grid cells are exact: the field is bilinear per cell, and each cell's
contribution is integrated analytically (never replaced by an average of the
cell's corner values).

Implementation: the texture-space triangle is partitioned by the map grid rows
and by the `v` values where the triangle's left/right boundaries cross grid
columns. On each piece the set of active cells is constant, so the
`u`-integrated field is an exact polynomial in `v` of degree ≤ 3. Its `v`
marginal is integrated exactly (double precision) to give each piece's mass,
and inverted with a bounded bisection to generate `v`; the conditional `u`
then samples the exact per-cell linear `u`-density. The distribution is built
once when the light is created and is shared read-only across threads; runtime
sampling consumes only the caller's two uniform numbers (no extra random
draws, no rejection loop, no fixed quadrature grid).

Edge cases: when the covered region is entirely black, `Z = 0`; position
sampling then falls back to a uniform area distribution over the triangle and
returns black radiance with zero power (no NaN).

`Sample_Li` converts the area density to solid-angle density by
`pA * r^2 / cos`; `Pdf_Li` returns that same density only for directions that
hit the triangle. `Sample_Le` samples position per `pA` and direction per the
usual (two-sided or one-sided) cosine distribution; `ray.time` is the caller's
`time`, and the ray origin uses the standard geometric offset. `Pdf_Le`
recovers the un-offset starting point, returns the position density without
folding in the direction density, and returns zero for queries off the
triangle.

## Tests

`src/tests/emissive_map.cpp` registers three groups with `TASK_CHECK`
assertions:

- `EmissiveMapGeometry` — grid values and `map[j*nu+i]` indexing on an oblique
  multi-cell UV triangle, default UVs, reflections and non-uniform transforms,
  the geometric normal, and emission through a plain `Interaction` and through
  a real ray/triangle intersection.
- `EmissiveMapDensity` — a hand-computable affine field, exact RGB power
  against an independently clipped-polygon integral, black sub-regions, the
  all-black fallback (zero power, uniform density, no NaN), and sampling
  frequency against exact per-bin masses.
- `EmissiveMapTransport` — area↔solid-angle density conversion and
  forward/reverse PDF round-trips, `ray.time`, one-sided vs two-sided
  emission, occlusion via `VisibilityTester`, and a full
  `GeometricPrimitive`/`Scene`/`EstimateDirect` reflected-radiance check on a
  diffuse receiver against an independent integral.

Numeric comparisons target relative errors around `2e-5` where an exact
reference exists; the statistical checks use a fixed sample sequence and print
the sample count and observed error so the bound is auditable.
