#include "fiber_renderer.h"
#include <cmath>
#include <limits>

namespace {
constexpr double MIN_T = 0.001;       // upstream minimum world hit distance
constexpr double FAR_T = 1.0e10;      // upstream scene horizon

bool finite_f(const vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

bool fiber_scene_valid(const FiberScene& scene) {
    for (const Fiber& f : scene.fibers) {
        if (!fiber_valid(f)) return false;
    }
    for (const Sphere& s : scene.occluders) {
        if (!finite_f(s.center) || !std::isfinite(s.radius) ||
            s.radius <= 0.0)
            return false;
    }
    return true;
}

dvec3 trace_fiber(const FiberScene& scene,
                  const dvec3& origin, const dvec3& dir,
                  int max_scatters,
                  FiberSampleStream& stream,
                  const BackgroundFn& background) {
    if (!fiber_scene_valid(scene)) return {0, 0, 0};
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        !std::isfinite(origin.z) ||
        !std::isfinite(dir.x) || !std::isfinite(dir.y) ||
        !std::isfinite(dir.z))
        return {0, 0, 0};
    double dn = dir.norm();
    if (!std::isfinite(dn) || std::fabs(dn - 1.0) > 1e-6) return {0, 0, 0};
    if (max_scatters < 0) return {0, 0, 0};

    dvec3 weight{1.0, 1.0, 1.0};
    dvec3 orig = origin;
    dvec3 d = dir;
    int skip_fiber = -1;   // only the fiber handled by the last event
    int scatters = 0;

    for (;;) {
        // Nearest sphere (original float intersection on double-converted
        // geometry would lose precision, so use the double variant of the
        // same quadratic).
        double sphere_t = FAR_T;
        bool hit_sphere = false;
        for (const Sphere& s : scene.occluders) {
            double t;
            if (ray_sphere_intersect_d(orig, d, to_dvec3(s.center),
                                       double(s.radius), MIN_T, FAR_T, t) &&
                t < sphere_t) {
                sphere_t = t;
                hit_sphere = true;
            }
        }

        // Nearest fiber side hit; exact ties go to the smaller id.
        FiberHit fh;
        bool hit_fiber = false;
        for (int i = 0; i < int(scene.fibers.size()); ++i) {
            if (i == skip_fiber) continue;
            FiberHit tmp;
            if (!fiber_intersect(scene.fibers[i], i, orig, d,
                                 MIN_T, FAR_T, tmp))
                continue;
            if (!hit_fiber || tmp.t < fh.t ||
                (tmp.t == fh.t && i < fh.fiber_id)) {
                fh = tmp;
                hit_fiber = true;
            }
        }

        // Exact same distance: the sphere wins.
        if (hit_sphere && (!hit_fiber || sphere_t <= fh.t))
            return {0, 0, 0};
        if (!hit_fiber) {
            dvec3 bg = background ? background(d) : dvec3{0, 0, 0};
            return cmul(bg, weight);
        }

        // A fiber was hit; the whole internal path is one material event.
        if (scatters >= max_scatters) return {0, 0, 0};

        const Fiber& f = scene.fibers[fh.fiber_id];
        dvec3 T = (f.B - f.A).normalized();
        dvec3 wo = d * (-1.0);
        FiberFrame frame = make_fiber_frame(T, wo);

        double u1, u2, u3;
        stream.next3(u1, u2, u3);
        // Geometry-derived h and r are passed straight through; hits are
        // never treated as if they landed on the fiber centerline.
        FiberSample smp = fiber_sample(f.material, frame, fh.h, f.radius,
                                       u1, u2, u3);
        if (!smp.ok || !(smp.pdf > 0.0)) return {0, 0, 0};

        for (int c = 0; c < 3; ++c) {
            double v = weight[c] * smp.K[c] / smp.pdf;
            if (!std::isfinite(v)) return {0, 0, 0};
            weight[c] = v;
        }

        orig = fh.point;
        d = smp.wi;
        skip_fiber = fh.fiber_id;
        ++scatters;
    }
}
