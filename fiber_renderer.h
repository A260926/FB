#ifndef FIBER_RENDERER_H
#define FIBER_RENDERER_H
#include "fiber.h"
#include <cstddef>
#include <functional>

// ---------------------------------------------------------------------------
// Fiber transport.
//
// A scene is a collection of finite open-cylinder fibers plus the original
// Spheres, which act as purely black occluders (a sphere hit contributes
// zero).  A caller-supplied background function gives the radiance of rays
// that escape without hitting anything.
//
// Bounces count fiber scattering events only.  When a fiber is hit and the
// bounce budget is already exhausted, the path returns zero.  Each material
// event wraps the whole internal cylinder path (entry, chord absorption,
// exit) once; the continuation ray starts at the current hit point and only
// skips the fiber that was just processed.  Other fibers and every sphere
// still participate, so after scattering off another fiber the same fiber
// may be encountered again.
//
// Ties at the 0.001 minimum world intersection distance are resolved
// deterministically: a sphere at exactly the same distance wins, otherwise
// the smaller fiber id wins.
// ---------------------------------------------------------------------------

struct FiberScene {
    const Fiber*  fibers  = nullptr;
    std::size_t   fiber_count = 0;
    const Sphere* spheres = nullptr;
    std::size_t   sphere_count = 0;
};

using BackgroundFn = std::function<dvec3(const dvec3 &dir)>;

// Trace one unit-length ray.  weight is the per-channel throughput carried by
// the caller (pass {1,1,1} for a camera ray).  samples is shared by the whole
// path; every fiber event consumes exactly three scalars.  max_bounds is the
// fiber-scattering limit (0 means a fiber hit immediately returns zero).
dvec3 trace_fiber_path(const FiberScene &scene, const dvec3 &orig,
                       const dvec3 &dir, const dvec3 &weight, int max_bounces,
                       SampleStream &samples, const BackgroundFn &background);

// Convenience wrapper starting with unit weight.
dvec3 trace_fiber_ray(const FiberScene &scene, const dvec3 &orig,
                      const dvec3 &dir, int max_bounces,
                      SampleStream &samples, const BackgroundFn &background);

#endif // FIBER_RENDERER_H
