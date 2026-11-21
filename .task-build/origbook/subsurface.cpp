#ifndef SUBSURFACE_CPP
#define SUBSURFACE_CPP
//==============================================================================================
// Implementation of the volumetric subsurface random walk. See subsurface.h for the contract.
//
// To the extent possible under law, the author(s) have dedicated all copyright and related and
// neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//==============================================================================================

#include "rtweekend.h"

#include "subsurface.h"
#include "material.h"

#include <cmath>
#include <stdexcept>
#include <utility>


namespace {

    // Marker material installed on every hit record produced by a subsurface_object so that the
    // camera can recognize the entry and hand the ray to the volumetric walk. The boundary's own
    // material is never used while the walk is inside the medium.
    class subsurface_material : public material {
      public:
        explicit subsurface_material(const subsurface_object& owner) : owner(owner) {}

        const subsurface_object& object() const { return owner; }

        // The camera reaches the walk through this virtual hook (see material.h), so the marker
        // runs the full random walk with the renderer's default sample stream.
        bool subsurface_walk(const ray& r_in, const hit_record& rec, int depth,
                             subsurface_result& out) const override {
            out = owner.random_walk(r_in, rec, depth, default_subsurface_sample);
            return true;
        }

        // The generic material path is never used for a subsurface hit.
        bool scatter(const ray&, const hit_record&, scatter_record&) const override {
            return false;
        }

      private:
        const subsurface_object& owner;
    };

    // Draw one sampler value and validate it. A non-finite result or anything outside the open
    // interval (0, 1) is a hard error so a broken callback can never spin the walk.
    double draw(subsurface_sampler& sampler) {
        const double u = sampler();
        if (!(u > 0.0 && u < 1.0))
            throw std::invalid_argument("subsurface sampler must return values strictly in (0,1)");
        return u;
    }

    // Uniform direction on the unit sphere from two (0,1) samples, with no rejection loop:
    // cos(theta) is uniform in (-1,1) and phi is uniform in [0,2pi).
    vec3 uniform_sphere_direction(subsurface_sampler& sampler) {
        const double z   = 2.0 * draw(sampler) - 1.0;
        const double phi = 2.0 * pi * draw(sampler);
        const double r   = std::sqrt(std::fmax(0.0, 1.0 - z*z));
        return vec3(r * std::cos(phi), r * std::sin(phi), z);
    }

    bool all_channels_zero(const vec3& v) {
        return v[0] == 0.0 && v[1] == 0.0 && v[2] == 0.0;
    }

    bool finite_vec(const vec3& v) {
        return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    }

}  // namespace


double default_subsurface_sample() {
    // Map rand() in [0, RAND_MAX] strictly into the open interval (0, 1), with no loop.
    return (std::rand() + 1.0) / (RAND_MAX + 2.0);
}


double subsurface_fresnel(double cos_theta_i, double eta_ratio) {
    // Unpolarized Fresnel reflectance. eta_ratio = eta_i / eta_t.
    cos_theta_i = std::fmin(std::fmax(cos_theta_i, 0.0), 1.0);
    const double sin2_theta_i = std::fmax(0.0, 1.0 - cos_theta_i*cos_theta_i);
    const double sin2_theta_t = eta_ratio*eta_ratio * sin2_theta_i;
    if (sin2_theta_t >= 1.0)
        return 1.0;  // Total internal reflection: reflection is the only possibility.

    const double cos_theta_t = std::sqrt(1.0 - sin2_theta_t);
    const double r_s = (eta_ratio*cos_theta_i - cos_theta_t)
                     / (eta_ratio*cos_theta_i + cos_theta_t);
    const double r_p = (cos_theta_i - eta_ratio*cos_theta_t)
                     / (cos_theta_i + eta_ratio*cos_theta_t);
    return 0.5 * (r_s*r_s + r_p*r_p);
}


subsurface_object::subsurface_object(std::shared_ptr<hittable> boundary,
                                     const subsurface_params& parameters)
  : boundary_(std::move(boundary)), params(parameters) {
    if (!boundary_)
        throw std::invalid_argument("subsurface_object requires a non-empty boundary");
    if (!finite_vec(params.sigma_s) || !finite_vec(params.sigma_a))
        throw std::invalid_argument("subsurface coefficients must be finite");
    if (!std::isfinite(params.eta) || params.eta <= 0.0)
        throw std::invalid_argument("subsurface eta must be finite and strictly positive");
    for (int c = 0; c < 3; ++c) {
        if (params.sigma_s[c] < 0.0 || params.sigma_a[c] < 0.0)
            throw std::invalid_argument("subsurface coefficients must be non-negative");
        if (params.sigma_s[c] + params.sigma_a[c] <= 0.0)
            throw std::invalid_argument("subsurface sigma_s + sigma_a must be positive per channel");
    }
    sigma_t = params.sigma_s + params.sigma_a;
    bbox = boundary_->bounding_box();
    marker = std::make_shared<subsurface_material>(*this);
}


const subsurface_object* subsurface_object::from(const std::shared_ptr<material>& m) {
    if (const auto* sm = dynamic_cast<const subsurface_material*>(m.get()))
        return &sm->object();
    return nullptr;
}


bool subsurface_object::hit(const ray& r, interval ray_t, hit_record& rec) const {
    if (!boundary_->hit(r, ray_t, rec))
        return false;
    rec.mat = marker;  // Hide the boundary's own material from external transport.
    return true;
}


subsurface_result subsurface_object::random_walk(const ray& r_in, const hit_record& entry,
                                                 int max_events,
                                                 subsurface_sampler sampler) const {
    if (!sampler)
        throw std::invalid_argument("subsurface random walk requires a sampler callback");

    subsurface_result out;
    if (max_events <= 0) {
        out.reason = subsurface_termination::budget;
        return out;
    }
    if (!entry.front_face)
        throw std::invalid_argument("subsurface walk must start at an outside (front-face) entry");

    // Each entry interface, volume collision and internal interface is one charged event shared
    // with the camera's depth budget. There is no separate internal cap, so repeated Fresnel
    // reflections cannot bypass the limit.
    int events = 0;
    auto charge_event = [&]() -> bool {
        if (events >= max_events)
            return false;
        ++events;
        return true;
    };

    const double ray_time = r_in.time();

    // ---- Event 1: the entry dielectric interface. ----
    if (!charge_event()) {
        out.events_used = events;
        return out;  // reason stays budget, weight stays zero
    }

    const vec3 entry_dir = unit_vector(r_in.direction());  // Work in world unit lengths.
    const double entry_cos =
        std::fmin(std::fmax(dot(-entry_dir, entry.normal), 0.0), 1.0);
    const double entry_r = subsurface_fresnel(entry_cos, 1.0 / params.eta);

    if (draw(sampler) < entry_r) {
        // External Fresnel reflection: hand a unit-weight external path back to the caller, which
        // continues it in the outside world. The walk never enters the medium.
        out.reason = subsurface_termination::reflected;
        out.weight = color(1, 1, 1);
        out.exit_ray = ray(entry.p, reflect(entry_dir, entry.normal), ray_time);
        out.events_used = events;
        return out;
    }

    // Transmission into the medium. The radiance convention carries (eta_i/eta_t)^2 = 1/eta^2.
    const double entry_eta_factor = 1.0 / (params.eta * params.eta);
    vec3 weight(entry_eta_factor, entry_eta_factor, entry_eta_factor);
    vec3 direction = refract(entry_dir, entry.normal, 1.0 / params.eta);  // Unit vector.
    point3 position = entry.p;

    while (true) {
        // Distance to the closed boundary along this unit direction, in world lengths. Only the
        // wrapped boundary is ever queried while the walk is inside the medium.
        hit_record boundary_hit;
        if (!boundary_->hit(ray(position, direction, ray_time),
                            interval(1e-9, infinity), boundary_hit)) {
            throw std::invalid_argument("subsurface boundary is not closed along the walk");
        }
        const double boundary_distance = boundary_hit.t;

        // Pick one RGB channel with equal probability and draw an exponential free-flight
        // distance with that channel's extinction coefficient. All lengths are world lengths, so
        // non-unit incoming directions, rigid placement and ray time cannot change the result.
        const int channel = static_cast<int>(3.0 * draw(sampler));
        const double flight = -std::log(draw(sampler)) / sigma_t[channel];

        vec3 transmittance;
        const double step = std::fmin(flight, boundary_distance);
        for (int c = 0; c < 3; ++c)
            transmittance[c] = std::exp(-sigma_t[c] * step);

        if (flight < boundary_distance) {
            // ---- Real volume collision. ----
            if (!charge_event()) {
                out.events_used = events;
                return out;  // budget: zero contribution
            }
            // Mixture denominator: mean over channels of the distance density sigma_t e^(-sigma_t s).
            // The full mixed denominator is used regardless of which channel drew the distance.
            double density_mix = 0.0;
            for (int c = 0; c < 3; ++c)
                density_mix += sigma_t[c] * transmittance[c];
            density_mix /= 3.0;

            for (int c = 0; c < 3; ++c)
                weight[c] *= transmittance[c] * params.sigma_s[c] / density_mix;

            position = position + flight * direction;
            direction = uniform_sphere_direction(sampler);  // Isotropic; no extra albedo factor.

            if (all_channels_zero(weight)) {
                out.reason = subsurface_termination::absorbed;
                out.events_used = events;
                return out;
            }
        } else {
            // ---- Reached the boundary before any collision. ----
            if (!charge_event()) {
                out.events_used = events;
                return out;  // budget: zero contribution
            }
            // Mixture denominator: mean over channels of the survival probability e^(-sigma_t d).
            const double survival_mix =
                (transmittance[0] + transmittance[1] + transmittance[2]) / 3.0;
            for (int c = 0; c < 3; ++c)
                weight[c] *= transmittance[c] / survival_mix;

            // The internal ray hits the far side, so the record normal points against the ray.
            const double internal_cos =
                std::fmin(std::fmax(dot(-direction, boundary_hit.normal), 0.0), 1.0);
            const double internal_r = subsurface_fresnel(internal_cos, params.eta);

            if (draw(sampler) < internal_r) {
                // Internal Fresnel reflection (the only branch under total internal reflection).
                direction = reflect(direction, boundary_hit.normal);
                position = boundary_hit.p;
            } else {
                // Refract out. The radiance convention carries (eta_i/eta_t)^2 = eta^2, which
                // cancels the entry factor for a path that leaves back into the same air. The
                // caller resumes the outside walk along this direction; the exit surface is not
                // re-diffused and the ray cannot return through the entry point.
                const vec3 outside_dir = refract(direction, boundary_hit.normal, params.eta);
                for (int c = 0; c < 3; ++c)
                    weight[c] *= params.eta * params.eta;

                out.reason = subsurface_termination::exited;
                out.weight = weight;
                out.exit_ray = ray(boundary_hit.p, outside_dir, ray_time);
                out.events_used = events;
                return out;
            }
        }
    }
}


#endif
