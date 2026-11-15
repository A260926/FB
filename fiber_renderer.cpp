#include "fiber_renderer.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr double HIT_EPS = 0.001;   // same minimum world intersection distance as the sphere code
constexpr double MAX_T   = 1.0e10;

dvec3 trace_recursive(const FiberScene &scene, const dvec3 &orig,
                      const dvec3 &dir, const dvec3 &weight, int max_bounces,
                      int skip_id, SampleStream &samples,
                      const BackgroundFn &background) {
    if (!all_finite(orig) || !all_finite(dir)) return {0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < scene.fiber_count; ++i)
        if (!scene.fibers[i].valid()) return {0.0, 0.0, 0.0};
    // Original sphere intersection; spheres are black occluders.
    bool sphere_hit = false;
    double sphere_t = MAX_T;
    const vec3 o32 = orig.to_vec3();
    const dvec3 dunit = (std::fabs(dir.norm() - 1.0) < 1e-9) ? dir : dir.normalized();
    const vec3 d32 = dunit.to_vec3();
    for (std::size_t i = 0; i < scene.sphere_count; ++i) {
        SphereHit sh = ray_sphere_intersect(o32, d32, scene.spheres[i]);
        if (sh.hit && sh.t < sphere_t) {
            sphere_hit = true;
            sphere_t   = sh.t;
        }
    }

    FiberHit fiber = intersect_fibers(scene.fibers, scene.fiber_count,
                                      orig, dunit, HIT_EPS, MAX_T, skip_id);

    // Exact-equal distance: the sphere wins; otherwise nearest surface wins.
    if (sphere_hit && (fiber.id < 0 || sphere_t <= fiber.t))
        return {0.0, 0.0, 0.0};
    if (fiber.id < 0)
        return cmul(weight, background(dunit));   // escaped: background at the ray weight

    // A fiber was hit.  Bounces count fiber scattering events only; with the
    // budget exhausted the path dies (and no samples are drawn).
    if (max_bounces <= 0)
        return {0.0, 0.0, 0.0};

    const Fiber *hit_fiber = nullptr;
    for (std::size_t i = 0; i < scene.fiber_count; ++i) {
        if (scene.fibers[i].id == fiber.id) { hit_fiber = &scene.fibers[i]; break; }
    }
    if (!hit_fiber) return {0.0, 0.0, 0.0};

    const FiberLobes lobes = fiber_attenuation(hit_fiber->material, fiber.h,
                                               hit_fiber->radius, fiber.frame);

    // One material event consumes exactly three [0, 1) samples.
    const double u = samples.draw();
    const double v = samples.draw();
    const double w = samples.draw();
    const FiberSample fs = fiber_sample(hit_fiber->material, lobes, fiber.frame, u, v, w);
    if (!(fs.pdf > 0.0) || !all_finite(fs.kernel) || !all_finite(fs.wi))
        return {0.0, 0.0, 0.0};

    dvec3 next_weight = cmul(weight,
        dvec3(fs.kernel.x/fs.pdf, fs.kernel.y/fs.pdf, fs.kernel.z/fs.pdf));
    if (!all_finite(next_weight)) return {0.0, 0.0, 0.0};

    // The whole internal path is this single event.  The continuation starts
    // at this hit point and skips only this fiber for the next segment.
    return trace_recursive(scene, fiber.point, fs.wi, next_weight,
                           max_bounces - 1, fiber.id, samples, background);
}
} // namespace

dvec3 trace_fiber_path(const FiberScene &scene, const dvec3 &orig,
                       const dvec3 &dir, const dvec3 &weight, int max_bounces,
                       SampleStream &samples, const BackgroundFn &background) {
    if (max_bounces < 0) return {0.0, 0.0, 0.0};
    return trace_recursive(scene, orig, dir, weight, max_bounces, -1,
                           samples, background);
}

dvec3 trace_fiber_ray(const FiberScene &scene, const dvec3 &orig,
                      const dvec3 &dir, int max_bounces,
                      SampleStream &samples, const BackgroundFn &background) {
    return trace_fiber_path(scene, orig, dir, dvec3(1.0, 1.0, 1.0),
                            max_bounces, samples, background);
}
