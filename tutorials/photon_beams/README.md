# Windowless photon beams in homogeneous fog

This directory contains `photon_beams::Renderer`, a small command-line/production
entry point for previewing short photon beams in a homogeneous participating
medium. It uses only C++17, the standard library, native Embree triangle scenes,
and the native `RTCPhotonBeamMap` BVH4/SAH index. There are no windows, GPU,
network, ISPC, or user-geometry callbacks in the new path.

## Native beam measure

For a beam with endpoints `from` and `to`, define

```text
L = |to - from|, w = (to - from) / L.
```

Its support is the **flat-ended finite cylinder**

```text
0 <= dot(X - from, w) <= L
|(X - from) - w dot(X - from, w)| < radius
```

with no hemispherical end caps. A query ray `X(t) = origin + t * direction`
uses its supplied, possibly non-unit, direction. The implementation
analytically intersects the cylinder and the axial slab with the query interval
`[tnear,tfar]`, then integrates over the resulting positive-length interval.
Tangencies and zero-width intervals have zero measure and contribute zero.

The per-beam RGB measure is exactly

```text
flux * sigma_s / (4*pi^2*radius^2)
* integral exp(-sigma_t * |direction| * (t-tnear)) * |direction| dt.
```

Thus `|direction| dt` is physical eye-ray length. The native map returns the sum
over beams and does not divide by photon count. The renderer divides its
progressive film sum by the cumulative number of emitted photons, including
emissions that produced no stored beam.

## Transport conventions

* The medium box has scalar `sigma_t` and `sigma_s`; the phase function is
  isotropic.
* Surface geometry consists of native Embree triangles with one RGB ideal-mirror
  reflectance.
* Photons start at the point light with flux equal to the light's total RGB
  power and use uniform spherical emission.
* The sampled free path competes with the nearest native surface distance and
  with entry/exit from the finite medium box.
* Only nonzero in-box path segments are stored. A segment runs from its box
  entry point to a volume collision, an in-box surface, or the box exit.
  Out-of-box vacuum segments are diagnostic-only and do not enter the beam pool.
  Crossing a box boundary does not consume an event.
* At a volume collision the throughput is multiplied by
  `sigma_s / sigma_t`; at a mirror it is multiplied by the RGB reflectance.
  Volume and mirror events share the clamped `1..32` event budget.
* Free-path sampling already represents photon survival to the endpoint:
  `P(S >= s) = exp(-sigma_t*s)`. Stored beam flux therefore must **not** be
  multiplied again by photon-path transmittance. The exponential attenuation
  along the camera/eye segment is still applied by the native query integral.
* After leaving the box, a photon can travel through vacuum and be reflected by
  an outside mirror back into the medium.

Each progressive round uses only its own beam map. Its radius is

```text
radius_round = baseRadius * (round + 1)^(-1/6).
```

The persistent film stores the unnormalized native query sums from all rounds;
`pixel(x,y)` divides by the cumulative emission count. `step(0)` is rejected and
leaves the existing film unchanged. Random identity is
`(settings.seed, zero-based round, photon index)`, so interleaving equally
configured instances gives the same films as running their rounds continuously.
Diagnostics only record random values already drawn by transport; reading them
does not consume random numbers.

## Geometry lifetime / freeze period

The `RTCScene` passed to the renderer must already be committed and must remain
geometrically frozen for the renderer's lifetime and for every `step()`/pixel
query. The renderer retains the scene (and device) handle, but retaining the
handle does not make concurrent geometry/buffer edits safe. Before changing
vertices, indices, attachments, or committing an update, destroy or release the
renderer and any beam maps that refer to the work. After modifying and
re-committing the scene, construct a new renderer for the new frozen geometry.

## Finite-radius bias

The finite beam radius is a density-estimation kernel approximation. Queries
integrate the exact flat cylinder support at that finite radius, but the result
is still a blurred estimate of the zero-radius photon-beam measure. Smooth
regions have the usual local `O(radius^2)` kernel bias; boundaries, singular
lighting, and close geometric features can have larger visible bias. The
shrinking progressive radius reduces this approximation with round number at
the standard bias/variance trade-off; this task does not claim that finite-radius
density bias has been eliminated.

## Diagnostics

`Renderer` exposes:

* `currentBeams`: the raw `RTCPhotonBeam` array used by the current native map;
* `currentSegments`: endpoint/type/storage diagnostics for in-box segments;
* `photons`: per-photon identity, directions, events, random free-path draws,
  and RGB flux before/after each event;
* `lastQueryStats`: summed native `nodeVisits`, `beamTests`, and
  `supportIntervals` for the last round's pixel queries;
* `completedRounds`, `emittedPhotons`, `lastRoundPhotons`, and `currentRadius`;
* `filmSum`: the cumulative unnormalized film.

## Build and run

From the repository root:

```sh
bash task_support/run.sh target
```

This configures the task build, compiles both native implementation files into
`libembree4.a`, forces the new headers into the task targets, runs the four
required test groups, runs `examples/photon_beams.cpp`, and then runs the
original triangle baseline.

The example can also be invoked through the generated target:

```sh
.task-build/taskexample /path/to/output-directory
# writes /path/to/output-directory/beams.ppm
```
