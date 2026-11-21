# Windowless photon beam rendering

`photon_beams::Renderer` (see `renderer.h`) is a headless progressive
photon-beam renderer built entirely on the native Embree API. It has no
window, network, GPU or TBB/ISPC dependency: only C++17, the standard
library and the in-repository native sources are used.

## Geometry freeze period

The `RTCScene` handed to the constructor is treated as **frozen for the
entire lifetime of the renderer**. It must be committed before the first
`step()` call and must not be attached to, released or have its geometry
buffers updated while the renderer exists. The renderer retains the scene
(and the device) itself, so the caller may release its own handles after
construction. Every ray cast — camera rays and photon path segments —
reads that one static triangle acceleration structure.

## Transport model and measures

* The world consists of a finite axis-aligned **medium box**. Inside the
  box the medium is homogeneous with scalar coefficients `sigma_t`
  (extinction) and `sigma_s` (scattering, `0 <= sigma_s <= sigma_t`) and an
  isotropic phase function `1/(4*pi)`. Outside the box the medium is
  vacuum.
* All surfaces are native triangles with one shared RGB reflectance and
  ideal specular reflection. Camera segments terminate at the nearest
  surface hit; the background is black.
* A point light emits photons uniformly over the sphere (solid angle
  measure), each carrying the full light power as its initial flux. The
  true free path `-log(1-xi)/sigma_t` competes against the nearest surface
  distance and the box exit distance.
* A **short beam** is stored for every non-zero segment spent inside the
  box, running from the segment entry point to the volume collision,
  surface hit or box exit. Vacuum segments are never stored and crossing
  the box boundary costs no event. At a volume collision the flux is
  multiplied by `sigma_s/sigma_t` and the photon is re-scattered
  isotropically; at a mirror the flux is multiplied by the reflectance and
  the direction follows the specular law. Volume and surface events share
  the single `maxEvents` budget (1..32). A photon that leaves the box may
  still be reflected back in by an exterior mirror.
* The sampled free path already accounts for the photon's survival
  probability, so neither storage nor lookup applies photon-side
  transmittance a second time. The eye ray keeps its own exponential
  transmittance `exp(-sigma_t * arc length)`.

### Beam radiance estimate and normalization

Each beam is a flat-ended finite cylinder of radius `r` with no
hemispherical end caps. Its normalized density kernel contributes
`1/(4*pi^2*r^2)` per unit path length, so for an eye ray the analytic
contribution of one beam is

    flux * sigma_s / (4*pi^2*r^2) *
        integral exp(-sigma_t * |d| * (t - tnear)) * |d| dt

over the ray interval that lies inside the cylinder. Overlapping beams
accumulate independently; hitting one beam never narrows the range of any
other. `pixel()` returns the sum of these unnormalized query results over
all rounds divided by the **cumulative number of emissions**, including
emissions that produced no stored beam. Results are linear RGB.

## Progressive rounds and finite-radius bias

Round `k` (zero based) uses radius

    r_k = radius * (k + 1)^(-1/6)

and keeps only that round's `RTCPhotonBeamMap`; the previous round's map is
released. The raw film is accumulated across rounds while the denominator
is the total number of emissions. The `-1/6` exponent is the standard
progressive photon-beam schedule: it drives the kernel radius to zero while
keeping the kernel-density estimate consistent as the number of photons
grows.

The finite radius is a **density-kernel approximation**. At any finite
round the estimate has kernel bias (beams are treated as cylinders of
non-zero radius rather than zero-width paths); this implementation does
not claim that bias is eliminated, only that it shrinks with the radius
schedule and that the measure is correctly normalized. The stochastic
Monte Carlo noise is separate and shrinks with the emission count.

## Diagnostics and reproducibility

`RoundDiagnostics` exposes the round's raw beam descriptors (with the
producing photon index and segment index), the full photon event stream
with per-event flux, native query counters (`nodeVisits`, `beamTests`,
`supportIntervals`) and the cumulative round/emission counts. Collecting
diagnostics consumes no random numbers.

All randomness is derived solely from `(seed, round index, photon index)`,
so two renderer instances with the same parameters produce identical
photons and films whether their `step()` calls are interleaved with other
instances or run back to back. `step(0)` is rejected and leaves the
existing film untouched.

## Example

`examples/photon_beams.cpp` builds a mirror box around an isotropic point
light, runs three progressive rounds and writes a binary PPM image:

    taskexample <output-directory>   # writes <output-directory>/beams.ppm
