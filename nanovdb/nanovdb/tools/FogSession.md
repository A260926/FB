# FogSession — incremental CPU fog integration for NanoVDB

`nanovdb::tools::FogSession` (in `nanovdb/nanovdb/tools/FogSession.h/.cc`)
integrates one world-space ray through several sparse NanoVDB fog volumes
directly on the CPU, walking the node hierarchy instead of expanding voxels.

It exists so a render thread can *yield*: work is split into bounded slices via
`advance(budget)`, and a partially integrated ray keeps its committed
transmittance and radiance.

## Public interface

```cpp
namespace nanovdb::tools {

struct FogVolume {
    const FloatGrid* grid;   // borrowed, must stay alive and unmodified
    double           extinction;
    Vec3d            emission;
};

struct FogResult {
    double transmittance;    // T of the committed interval
    Vec3d  radiance;         // C (+ T*background once complete)
};

class FogSession {
public:
    FogSession(std::vector<FogVolume> volumes,
               const math::Ray<double>& ray,
               const Vec3d& background);

    FogSession(const FogSession&) = delete;              // copy disabled
    FogSession& operator=(const FogSession&) = delete;
    FogSession(FogSession&&) noexcept;                   // move enabled
    FogSession& operator=(FogSession&&) noexcept;

    bool        advance(std::size_t budget); // true once the ray is complete
    FogResult   result() const;
    std::size_t spanSteps() const;
};

} // namespace nanovdb::tools
```

## Optics

* A voxel is the half-open index box `[i, i+1)`.
* Only **active** voxels and **active constant tiles** contribute. Inactive
  values are vacuum even if their stored value is non-zero.
* Density is `max(value, 0)`.
* For a constant world-space segment of length `L` the overlapping volumes
  combine as
  `sigma = Σ density·extinction`, `j = Σ density·emission`, and then
  `C += T·j·(1-exp(-sigma·L))/sigma`, `T *= exp(-sigma·L)` — with
  `C += T·j·L` when `sigma == 0`.
* The completed result is `radiance = C + T·background`. An incomplete result
  returns the committed `C` and `T` **without** the background term.
* Integration runs near to far; equal-distance boundaries take effect
  together and no epsilon discards thin non-zero intervals.

## Traversal and resumability

* The supported grid mapping is a **positive uniform scale plus translation**
  (what the bundled builders and primitives produce). Each volume may use
  different scale and translation.
* A ray is parameterized in world space; its direction need not be
  normalized, and the world-space path length is independent of that length.
  The `[t0, t1]` interval must be finite.
* Each volume keeps a **cursor** for its current constant hierarchy region.
  Cursors are advanced only far enough to answer the next world-space
  boundary, so the session never expands a grid densely, never uses a fixed
  step size, and never collects the whole ray's events up front.
* **Persistent memory** grows with the number of volumes (and tree depth),
  not with the number of voxels crossed.
* `advance(budget)` performs at most `budget` hierarchy span steps and
  returns whether the ray is fully integrated. `budget == 0` makes no
  progress. Once complete, further calls return `true` and leave the result
  unchanged.
* `spanSteps()` counts hierarchy constant intervals **actually fetched or
  advanced**, including vacuum intervals. Construction performs no traversal
  (`spanSteps() == 0`).
* If a budget stops in the middle of a set of equal-distance events, the
  pending cursors are retained by the session; no event is integrated early
  and none is dropped.
* Sessions share no mutable state, so two sessions may be advanced
  interleaved.

For an independently built block of active uniform density with
`x ∈ [0, 4096)`, `y, z ∈ [0, 128)` and a ray through its interior, a single
volume consumes fewer than 64 span steps: the 32 constant `128³` tiles are
actually used, not skipped as empty space.

## Example

`nanovdb/nanovdb/examples/ex_raytrace_fog_volume/fog_session_example.cc` is a
windowless, dependency-free multi-volume demo:

```bash
c++ -std=c++17 -I nanovdb fog_session_example.cc \
    nanovdb/nanovdb/tools/FogSession.cc -o fog_session_example
./fog_session_example
```

The original CPU example (`ex_raytrace_fog_volume/nanovdb.cu`) uses the same
session in its host branch. Its `runNanoVDB` signature, PFM output and CUDA
branch are unchanged.
