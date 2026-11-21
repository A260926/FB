#ifndef SUBSURFACE_H
#define SUBSURFACE_H
//==============================================================================================
// Subsurface scattering via a true volumetric random walk inside a closed boundary.
//
// A subsurface_object owns an already-placed closed hittable boundary (a sphere, a convex box
// built from six quads, or either of those wrapped in rotate_y/translate) together with the
// participating-medium parameters (RGB scattering/absorption coefficients and the index of
// refraction relative to the outside air). The boundary's own material never participates in
// internal transport; only its geometry is queried while the walk is inside it.
//
// Lifetime: the object keeps the boundary alive through a shared_ptr, so the boundary only has
// to exist for the duration of the construction call. The parameter struct is copied and the
// sampling callback is borrowed for the duration of a walk only, so neither has to outlive the
// call. A walk charges one scattering event per entry interface, volume collision and internal
// interface against the camera's shared depth budget.
//
// To the extent possible under law, the author(s) have dedicated all copyright and related and
// neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//==============================================================================================

#include "hittable.h"
#include "ray.h"
#include "vec3.h"

#include <functional>
#include <memory>


class material;


// Participating-medium coefficients, measured per unit of world length.
struct subsurface_params {
    vec3 sigma_s;  // RGB scattering coefficients, each component >= 0
    vec3 sigma_a;  // RGB absorption coefficients, each component >= 0
    double eta;    // Index of refraction relative to the outside air, > 0

    subsurface_params() : sigma_s(0, 0, 0), sigma_a(0, 0, 0), eta(1.0) {}

    subsurface_params(const vec3& sigma_s, const vec3& sigma_a, double eta)
      : sigma_s(sigma_s), sigma_a(sigma_a), eta(eta) {}
};


// Why a subsurface random walk stopped.
enum class subsurface_termination {
    reflected, // Fresnel reflection at the entry interface; the external path continues, weight 1.
    exited,    // The walk refracted out through the boundary (see result.exit_ray).
    absorbed,  // The spectral weight collapsed to zero before the walk could leave.
    budget     // The shared scattering-event budget ran out; the contribution is zero.
};


// The outcome of a single random walk.
struct subsurface_result {
    ray exit_ray;                       // Reflected ray on `reflected`, refracted ray on `exited`.
    vec3 weight = vec3(0, 0, 0);        // Spectral (RGB) transport weight.
    int events_used = 0;                // Scattering events charged against the budget.
    subsurface_termination reason = subsurface_termination::budget;
};


// A source of uniform samples in the open interval (0, 1). Returning a value outside (0, 1),
// including a non-finite value, makes the walk throw std::invalid_argument instead of looping.
using subsurface_sampler = std::function<double()>;


// Default sampler: one uniform (0,1) draw from the renderer's global random stream.
double default_subsurface_sample();


class subsurface_object : public hittable {
  public:
    // Takes shared ownership of the already-placed closed boundary. Throws std::invalid_argument
    // on a null boundary, non-finite or negative coefficients, a non-positive per-channel total
    // extinction, or a non-finite/non-positive eta.
    subsurface_object(std::shared_ptr<hittable> boundary, const subsurface_params& params);

    // The marker material holds a reference back to this object, so copies would dangle; the
    // object is intended to live behind a shared_ptr and is not copyable.
    subsurface_object(const subsurface_object&) = delete;
    subsurface_object& operator=(const subsurface_object&) = delete;

    // Recognizes the marker material installed by subsurface_object::hit. Returns the owning
    // subsurface object, or null when the hit belongs to an ordinary material.
    static const subsurface_object* from(const std::shared_ptr<material>& mat);

    // Stand-alone random walk for an outside ray `r` whose entry hit record is `rec`. The walk
    // queries only the wrapped boundary; the caller resumes outside-world transport along
    // result.exit_ray. `max_events` is the shared depth budget and every entry interface, volume
    // collision and internal interface counts as one event. Throws std::invalid_argument when the
    // sampler is missing or returns a value outside (0, 1), or when the boundary is not closed.
    subsurface_result random_walk(const ray& r, const hit_record& rec, int max_events,
                                  subsurface_sampler sampler) const;

    // Hittable interface: exposes the wrapped boundary so the object can be placed in a world.
    // Internal transport queries the boundary directly and never routes through this hit().
    bool hit(const ray& r, interval ray_t, hit_record& rec) const override;
    aabb bounding_box() const override { return bbox; }

    const subsurface_params& parameters() const { return params; }
    const hittable& boundary() const { return *boundary_; }

  private:
    std::shared_ptr<hittable> boundary_;
    std::shared_ptr<material> marker;  // Installed on external hit records; references *this.
    subsurface_params params;
    vec3 sigma_t;  // sigma_s + sigma_a per channel, always strictly positive per component
    aabb bbox;
};


// Unpolarized dielectric Fresnel reflectance for an incident direction whose angle with the
// outward surface normal has cosine `cos_theta_i` (clamped to [0, 1]). `eta_ratio` is
// eta_i / eta_t. Exact Fresnel equations, never Schlick's approximation.
double subsurface_fresnel(double cos_theta_i, double eta_ratio);


#endif
