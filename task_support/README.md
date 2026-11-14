# Transient (time-resolved) volumetric path tracing

This directory supports the offline experiment in which a short light pulse
propagates through homogeneous fog and the arriving energy is recorded as a
function of flight time.  The implementation reuses pbrt-v3's volumetric path
tracer transport rules but keeps every estimator contribution tagged with its
physical propagation time instead of folding them into one steady-state
spectrum.

## Files

- `../src/core/transient.h`, `../src/core/transient.cpp`
  - `TemporalPulse`, `TimedContribution`, `TraceTransient`, the
    `TransientFilm` / transient film tile, and scene-validation helpers.
- `../src/integrators/transientvolpath.h`,
  `../src/integrators/transientvolpath.cpp`
  - The `transientvolpath` integrator and its parallel tile render loop.
- `../src/core/light.h`, `../src/lights/point.*`, `../src/lights/diffuse.*`
  - Finite lights carry an optional normalized `TemporalPulse`.
- `../src/media/homogeneous.*`
  - Adds the group index used for propagation speed.
- `../src/core/film.*`
  - Virtual hooks so the steady `image` film and the `transient` film share
    spatial filtering, crop handling, scale, tile merging and locking.
- `../src/core/api.cpp`, `../src/core/transform.h`
  - Scene-format wiring, the new integrator/film names, and rejection logic.
- `../scenes/transient-fog.pbrt`
  - A fully self-contained example scene (geometry, media and pulses inline).
- `../src/tests/transient.cpp`
  - GoogleTest suites `TransientPulse`, `TransientTransport`, `TransientFilm`.

## Units

- World-space distances are interpreted through the integrator parameter
  `metersperunit` (default `1`).  It must be finite and strictly positive.
- Internal flight time is accumulated in **seconds** as `double`.
- Film time parameters (`tstart`, `binwidth`) and light pulse nodes
  (`pulsetimes`) are expressed in **nanoseconds** and converted to seconds.
- The exact speed of light is `299792458 m/s`.
- For a geometric segment of length `L` (world units) inside a medium with
  group index `n_g`, the added flight time is
  `L * metersperunit * n_g / 299792458` seconds.  Vacuum (a ray with no
  medium) has group index one.
- The medium parameter `groupindex` (default `1`, must be finite and `>= 1`)
  is the *group* index controlling propagation speed.  It is deliberately
  independent of the surface BSDF phase index `eta`.
- `sigma_a` / `sigma_s` keep their original pbrt meaning and units
  (per world unit); the medium sampling, transmittance and throughput rules
  are unchanged from `volpath`.

## Pulse model

`TemporalPulse(timesSeconds, values)` stores a piecewise-linear, compactly
supported emission shape.  It is normalized so its integral over the whole
support equals one; therefore scaling the pulse shape never changes the total
emitted energy and the light's `I` (PointLight) / `L` (DiffuseAreaLight) keep
their original meaning as the full-pulse integrated intensity / radiance.

Scene parameters (both optional, but must be supplied together):

- `"float pulsetimes" [ ... ]` nanoseconds, finite, non-negative, strictly
  increasing, at least two nodes.
- `"float pulsevalues" [ ... ]` finite, non-negative, equal count, with a
  strictly positive trapezoidal integral.

If both are omitted the default pulse is the triangular nodes
`[0, 1, 2] ns` with values `[0, 1, 0]`.  Invalid input throws
`std::invalid_argument` from the C++ constructor; the scene creation layer
converts that into an `Error` and aborts before rendering (no output file).

`TemporalPulse::Integral(a, b)` and `ShiftedIntegral(a, b, delay)` evaluate
the exact piecewise-linear interval mass; no time sampling or numerical grid
is used.

## Public interface

```cpp
struct TimedContribution {
    Spectrum value;        // full-time-integrated estimator contribution
    double   flightTime;   // camera -> emitting endpoint, seconds
    const Light* emitter;  // the light whose pulse shapes this energy
};

std::vector<TimedContribution> TraceTransient(
    const RayDifferential& ray, const Scene& scene, Sampler& sampler,
    MemoryArena& arena, int maxDepth, Float rrThreshold,
    double metersPerUnit);

class TemporalPulse {
  public:
    TemporalPulse(std::vector<double> timesSeconds,
                  std::vector<double> values);
    double Integral(double beginSeconds, double endSeconds) const;
    double ShiftedIntegral(double beginSeconds, double endSeconds,
                           double delaySeconds) const;
};

class TransientFilm : public Film {
  public:
    // bins time bins; tStartNs / binWidthNs are in nanoseconds.
    std::vector<std::array<double, 3>> GetTransientRGB(const Point2i&) const;
};
```

Bin `k` covers
`[tstart + k*binwidth, tstart + (k+1)*binwidth)` (half open).  The energy a
contribution places in a bin is `value * pulse.ShiftedIntegral(bin0, bin1,
flightTime)`; the bin width is not divided out.  Each camera sample adds to a
pixel's spatial filter denominator exactly once, regardless of how many
contributions or bins it spans, whether it is fully outside the time window,
or whether the camera ray weight is zero; the ray weight multiplies the
numerator only.

`Film::GetTransientRGB` returns the merged, spatially normalized, scale-applied
linear RGB per bin (no tonemapping or gamma).  The CSV written by
`TransientFilm::WriteImage` has the fixed header
`x,y,bin,t0_ns,t1_ns,r,g,b`, one row per cropped pixel per bin, ordered by
`y`, then `x`, then `bin`, and holds identical values to the in-memory
readout.

## Transport rules and restrictions

`TraceTransient` mirrors `VolPathIntegrator`: the same `maxdepth`,
Russian-roulette threshold and throughput rule, medium sampling, BSDF/phase
sampling, MIS weights and uniform light selection.  No extra random numbers
are introduced.  The two direct-lighting MIS branches are kept separate and
each retains its own emitting endpoint, transmittance and flight time;
specular continuations that reach an area light are also recorded.  Spawn-ray
self-intersection offsets affect only intersection tests, never the physical
distances, and non-unit ray directions cannot change timing.  Material-less
medium boundaries switch media without consuming scattering depth and retain
the accumulated optical path length.  `Ray::time` (the shutter time) is never
used as propagation time.

Before rendering, `transientvolpath` rejects (with `Error`, no partial
output): animated camera/geometry/light transforms, infinite or non-finite
light types, BSSRDF materials, non-homogeneous media, a non-RGB build, a
non-transient film, invalid pulse / film / medium / integrator parameters,
and any `maxsampleluminance` on the transient film.  The steady integrators
likewise reject a transient film.

## Build and run

All third-party dependencies are vendored under `../src/ext`; no downloads or
services are required.

```sh
# Configure and build pbrt_exe and pbrt_test (uses fixed local sources).
bash task_support/install.sh

# Build, then run the three transient suites plus the steady baseline.
bash task_support/run.sh target
```

Render the example scene:

```sh
./build/task/pbrt scenes/transient-fog.pbrt
# writes transient-fog.csv next to the scene (override with --outfile).
```
