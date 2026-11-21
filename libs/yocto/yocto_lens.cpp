//
// Implementation for Yocto/Lens.
//

//
// LICENSE:
//
// Copyright (c) 2016 -- 2022 Fabio Pellacini
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
//

#include "yocto_lens.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "yocto_scene.h"
#include "yocto_sampling.h"

// -----------------------------------------------------------------------------
// IMPLEMENTATION
// -----------------------------------------------------------------------------
namespace yocto {

namespace {

// Small double precision vector used internally for robust ray tracing and
// for mechanical focus solves. The public API stays in float.
struct dvec3 {
  double x = 0, y = 0, z = 0;
};

inline dvec3 operator+(const dvec3& a, const dvec3& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline dvec3 operator-(const dvec3& a, const dvec3& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline dvec3 operator*(const dvec3& a, double b) {
  return {a.x * b, a.y * b, a.z * b};
}
inline dvec3 operator/(const dvec3& a, double b) {
  return {a.x / b, a.y / b, a.z / b};
}
inline double dot(const dvec3& a, const dvec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline double length(const dvec3& a) { return std::sqrt(dot(a, a)); }
inline dvec3 normalize(const dvec3& a) {
  auto l = length(a);
  return l > 0 ? a / l : dvec3{0, 0, 1};
}
inline bool isfinite(const dvec3& a) {
  return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

inline dvec3 to_dvec(const vec3f& a) {
  return {(double)a.x, (double)a.y, (double)a.z};
}
inline vec3f to_vec(const dvec3& a) {
  return {(float)a.x, (float)a.y, (float)a.z};
}

// Result of an internal trace, in double precision.
struct dtrace {
  bool  valid         = false;
  dvec3 origin        = {0, 0, 0};
  dvec3 direction     = {0, 0, 1};
  double transmittance = 0;
  int   last_interface = -1;
};

constexpr auto lens_ray_eps  = 1e-6;
constexpr auto lens_air_eta  = 1.0;

// Solves the ray/sphere intersection. Returns the nearest valid parameter
// whose point lies on the half sphere containing the vertex, or false.
// The sphere is centered at (0,0,z+R) with radius |R|. The vertex is at
// (0,0,z), so the accepted half satisfies (p.z - c.z) * R < 0.
bool intersect_lens_sphere(const dvec3& origin, const dvec3& direction,
    double z, double R, double r, double& t_out) {
  auto center = dvec3{0, 0, z + R};
  auto radius = std::abs(R);
  auto oc     = origin - center;
  auto b      = 2 * dot(oc, direction);
  auto c      = dot(oc, oc) - radius * radius;
  auto disc   = b * b - 4 * c;
  if (disc < 0) return false;
  auto sdisc = std::sqrt(disc);
  auto t0    = (-b - sdisc) / 2;
  auto t1    = (-b + sdisc) / 2;
  if (t0 > t1) std::swap(t0, t1);
  auto best = std::numeric_limits<double>::infinity();
  for (auto t : {t0, t1}) {
    if (!(t >= -lens_ray_eps) || !std::isfinite(t)) continue;
    auto p = origin + direction * t;
    // half sphere containing the vertex
    if (!((p.z - center.z) * R < 0)) continue;
    // effective clear radius
    if (p.x * p.x + p.y * p.y > r * r) continue;
    best = std::min(best, t);
  }
  if (!std::isfinite(best)) return false;
  t_out = best;
  return true;
}

// Solves the ray/plane intersection at z = plane_z, checking the effective
// radius. Returns the nearest valid parameter.
bool intersect_lens_plane(const dvec3& origin, const dvec3& direction,
    double plane_z, double r, double& t_out) {
  if (std::abs(direction.z) < 1e-14) return false;
  auto t = (plane_z - origin.z) / direction.z;
  if (!(t >= -lens_ray_eps) || !std::isfinite(t)) return false;
  auto p = origin + direction * t;
  if (p.x * p.x + p.y * p.y > r * r) return false;
  return t_out = t, true;
}

// Unpolarized Fresnel transmittance for a dielectric interface.
double fresnel_transmittance(double eta_before, double eta_after,
    double cos_incident, double cos_transmitted) {
  auto rs_num = eta_before * cos_incident - eta_after * cos_transmitted;
  auto rs_den = eta_before * cos_incident + eta_after * cos_transmitted;
  auto rp_num = eta_before * cos_transmitted - eta_after * cos_incident;
  auto rp_den = eta_before * cos_transmitted + eta_after * cos_incident;
  auto rs     = rs_den != 0 ? rs_num / rs_den : 0.0;
  auto rp     = rp_den != 0 ? rp_num / rp_den : 0.0;
  return 1 - 0.5 * (rs * rs + rp * rp);
}

// Refracts a direction at a spherical interface. Returns false on total
// internal reflection. The normal is oriented against the incoming direction.
bool refract_lens(const dvec3& direction, dvec3 normal, double eta_before,
    double eta_after, dvec3& result, double& fresnel) {
  if (dot(normal, direction) > 0) normal = normal * -1.0;
  auto cos_incident = -dot(normal, direction);
  if (!(cos_incident > 0)) return false;
  auto ratio = eta_before / eta_after;
  auto k     = 1 - ratio * ratio * (1 - cos_incident * cos_incident);
  if (k < 0) {
    if (k > -1e-12) {
      k = 0;
    } else {
      return false;  // total internal reflection
    }
  }
  auto cos_transmitted = std::sqrt(k);
  result = normalize(direction * ratio +
                     normal * (ratio * cos_incident - cos_transmitted));
  fresnel = fresnel_transmittance(
      eta_before, eta_after, cos_incident, cos_transmitted);
  return std::isfinite(fresnel) && fresnel >= 0 && isfinite(result);
}

// Traces a ray through the lens in double precision. The aperture stop and
// every interface are treated as surfaces; at each step the nearest valid
// intersection along the ray is used. Traveling toward the scene crosses the
// aperture first and the interfaces in increasing order, while traveling back
// toward the film crosses them in decreasing order and then the aperture.
// Missing the aperture, missing an effective radius, missing an intersection,
// or undergoing total internal reflection marks the trace as blocked; such
// rays are never resampled.
dtrace trace_lens_d(const lens_config& lens, const dvec3& origin_,
    const dvec3& direction_) {
  auto result    = dtrace{};
  auto direction = normalize(direction_);
  if (!isfinite(origin_) || !isfinite(direction) ||
      !std::isfinite(length(direction)) || length(direction) == 0) {
    return result;
  }
  // The public tracer accepts rays traveling in either direction, but the
  // camera and focus solve use forward rays only. For backward rays the
  // aperture is crossed last; the order of the surfaces is learned from the
  // first intersection instead of the direction sign.
  auto forward = direction.z >= 0;

  auto origin         = origin_;
  auto count          = (int)lens.interfaces.size();
  auto transmittance  = 1.0;
  auto aperture_cross = false;
  auto crossed        = vector<bool>(count, false);
  auto crossed_count  = 0;
  auto remaining      = count + 1;  // interfaces plus the aperture

  while (remaining > 0) {
    auto best_t     = std::numeric_limits<double>::infinity();
    auto best_kind  = -1;  // -2 for the aperture, >= 0 for interfaces
    auto best_index = -1;

    if (!aperture_cross) {
      auto t = 0.0;
      if (intersect_lens_plane(origin, direction, 0, lens.aperture, t) &&
          t < best_t) {
        best_t    = t;
        best_kind = -2;
      }
    }
    for (auto i = 0; i < count; i++) {
      if (crossed[i]) continue;
      auto& iface = lens.interfaces[i];
      auto  t     = 0.0;
      auto  hit   = false;
      if (iface.R == 0) {
        hit = intersect_lens_plane(origin, direction, iface.z, iface.r, t);
      } else {
        hit = intersect_lens_sphere(
            origin, direction, iface.z, iface.R, iface.r, t);
      }
      if (hit && t < best_t) {
        best_t     = t;
        best_kind  = i;
        best_index = i;
      }
    }

    if (!std::isfinite(best_t)) return result;  // nothing left to hit

    // In backward mode the aperture must be the very last crossing, so a
    // surface found after the aperture is a geometric inconsistency.
    if (best_kind == -2 && !forward) {
      if (crossed_count != count) return result;
    }
    if (best_kind >= 0 && !forward && aperture_cross) return result;

    auto p = origin + direction * best_t;

    if (best_kind == -2) {
      // The aperture only blocks; it never refracts.
      aperture_cross = true;
      origin         = p;
      remaining--;
      continue;
    }

    auto& iface = lens.interfaces[best_index];
    // Determine the medium on each side from the travel direction. Camera
    // rays go toward +z; reversing a traced ray goes toward -z.
    auto eta_before = forward
                          ? ((best_index == 0)
                                    ? lens_air_eta
                                    : (double)lens.interfaces[best_index - 1].eta)
                          : (double)iface.eta;
    auto eta_after = forward
                         ? (double)iface.eta
                         : ((best_index == 0)
                                   ? lens_air_eta
                                   : (double)lens.interfaces[best_index - 1].eta);
    if (iface.R != 0) {
      auto center = dvec3{0, 0, iface.z + (double)iface.R};
      auto normal = normalize(p - center);
      auto fresnel = 0.0;
      if (!refract_lens(direction, normal, eta_before, eta_after, direction,
              fresnel)) {
        return result;
      }
      transmittance *= fresnel;
    }
    crossed[best_index] = true;
    crossed_count++;
    origin    = p;
    remaining--;
  }

  result.valid         = true;
  result.origin        = origin;
  result.direction     = direction;
  result.transmittance = transmittance;
  result.last_interface = count - 1;
  return result;
}

// Nearest positive x intercept of the traced ray at z = focus_z.
bool focus_intercept(const lens_config& lens, double film_distance,
    double focus_z, double& intercept) {
  auto h        = (double)lens.aperture * 1e-3;
  auto origin   = dvec3{0, 0, -film_distance};
  auto target   = dvec3{h, 0, 0};
  auto traced   = trace_lens_d(lens, origin, target - origin);
  if (!traced.valid) return false;
  if (!(traced.direction.z > 0)) return false;
  auto t = (focus_z - traced.origin.z) / traced.direction.z;
  if (!(t > 0) || !std::isfinite(t)) return false;
  auto p = traced.origin + traced.direction * t;
  intercept = p.x;
  return std::isfinite(intercept);
}

}  // namespace

// -----------------------------------------------------------------------------
// VALIDATION
// -----------------------------------------------------------------------------
// Validates the geometry of a lens, excluding the film distance. Focusing uses
// this so it can determine the film distance even when the configuration does
// not have one yet.
static bool validate_lens_geometry(const lens_config& lens, string& error) {
  if (lens.interfaces.empty()) {
    // A camera without a lens group keeps using the legacy model.
    return true;
  }
  if (!std::isfinite(lens.aperture) || !(lens.aperture > 0)) {
    error = "invalid lens aperture";
    return false;
  }
  auto prev_z   = 0.0f;
  auto prev_eta = (float)lens_air_eta;
  for (auto i = 0; i < (int)lens.interfaces.size(); i++) {
    auto& iface = lens.interfaces[i];
    if (!std::isfinite(iface.z) || !std::isfinite(iface.R) ||
        !std::isfinite(iface.r) || !std::isfinite(iface.eta)) {
      error = "non-finite lens interface";
      return false;
    }
    if (!(iface.z > prev_z)) {
      error = "lens interfaces not strictly increasing";
      return false;
    }
    if (!(iface.r > 0)) {
      error = "invalid lens interface radius";
      return false;
    }
    if (!(iface.eta > 0)) {
      error = "invalid lens refractive index";
      return false;
    }
    if (iface.R == 0) {
      // a plane diaphragm does not refract, so it must not change medium
      if (std::abs(iface.eta - prev_eta) >
          1e-6f * std::max(1.0f, std::abs(prev_eta))) {
        error = "plane lens interface changes refractive index";
        return false;
      }
    } else {
      if (!(iface.r < std::abs(iface.R))) {
        error = "invalid lens interface curvature radius";
        return false;
      }
    }
    prev_z   = iface.z;
    prev_eta = iface.eta;
  }
  if (std::abs(prev_eta - (float)lens_air_eta) >
      1e-6f * std::max(1.0f, std::abs(prev_eta))) {
    error = "lens does not exit into air";
    return false;
  }
  return true;
}

bool validate_lens(const lens_config& lens, string& error) {
  if (!validate_lens_geometry(lens, error)) return false;
  if (lens.interfaces.empty()) return true;
  if (!std::isfinite(lens.film_distance) || !(lens.film_distance > 0)) {
    error = "invalid lens film distance";
    return false;
  }
  return true;
}

bool validate_lens(const lens_config& lens) {
  auto error = string{};
  return validate_lens(lens, error);
}

// -----------------------------------------------------------------------------
// SINGLE RAY TRACING
// -----------------------------------------------------------------------------
lens_trace trace_lens_ray(const lens_config& lens, const ray3f& ray) {
  auto traced = trace_lens_d(lens, to_dvec(ray.o), to_dvec(ray.d));
  auto result = lens_trace{};
  result.valid         = traced.valid;
  result.origin        = to_vec(traced.origin);
  result.direction     = to_vec(traced.direction);
  result.transmittance = (float)traced.transmittance;
  result.last_interface = traced.last_interface;
  return result;
}

// -----------------------------------------------------------------------------
// MECHANICAL FOCUS
// -----------------------------------------------------------------------------
lens_focus_result solve_lens_focus(const lens_config& lens, float focus_z,
    float film_distance_min, float film_distance_max) {
  auto result = lens_focus_result{};
  auto error  = string{};
  // Focusing determines the film distance, so only the lens geometry needs to
  // be valid here, not the configuration's current film distance.
  if (!validate_lens_geometry(lens, error)) {
    result.error = error;
    return result;
  }
  if (lens.interfaces.empty()) {
    result.error = "no lens group configured";
    return result;
  }
  if (!std::isfinite(focus_z) || !(focus_z > lens.interfaces.back().z)) {
    result.error = "invalid focus plane";
    return result;
  }
  if (!std::isfinite(film_distance_min) || !std::isfinite(film_distance_max) ||
      !(film_distance_min > 0) || !(film_distance_max > film_distance_min)) {
    result.error = "invalid film distance interval";
    return result;
  }

  auto gmin = (double)film_distance_min;
  auto gmax = (double)film_distance_max;
  auto fz   = (double)focus_z;

  auto f_lo = 0.0, f_hi = 0.0;
  if (!focus_intercept(lens, gmin, fz, f_lo)) {
    result.error = "focus trace blocked at interval start";
    return result;
  }
  if (!focus_intercept(lens, gmax, fz, f_hi)) {
    result.error = "focus trace blocked at interval end";
    return result;
  }

  constexpr auto intercept_tol = 1e-8;
  if (std::abs(f_lo) <= intercept_tol) {
    result.ok            = true;
    result.film_distance = film_distance_min;
    return result;
  }
  if (std::abs(f_hi) <= intercept_tol) {
    result.ok            = true;
    result.film_distance = film_distance_max;
    return result;
  }
  if (!(f_lo * f_hi < 0)) {
    result.error = "focus interval does not bracket a root";
    return result;
  }

  auto lo = gmin, hi = gmax;
  auto lo_value = f_lo;
  auto best     = 0.0, best_f = std::numeric_limits<double>::infinity();
  auto found    = false;
  for (auto iter = 0; iter < 200; iter++) {
    auto mid = 0.5 * (lo + hi);
    if (!(mid > lo && mid < hi)) break;
    auto f_mid = 0.0;
    if (!focus_intercept(lens, mid, fz, f_mid)) {
      result.error = "focus trace blocked during solve";
      return result;
    }
    if (std::abs(f_mid) < best_f) {
      best_f = std::abs(f_mid);
      best   = mid;
      found  = true;
    }
    if (std::abs(f_mid) <= 1e-11) break;
    if (f_mid * lo_value > 0) {
      lo      = mid;
      lo_value = f_mid;
    } else {
      hi = mid;
    }
    if (hi - lo < 1e-15) break;
  }
  if (!found) {
    result.error = "focus did not converge";
    return result;
  }
  if (best_f > intercept_tol) {
    result.error = "focus did not converge";
    return result;
  }

  // The API returns a float film distance. Pick the nearby float whose
  // intercept is smallest so that the returned value satisfies the tolerance
  // when evaluated through the production trace.
  auto best_g = (float)best;
  auto best_gi = (double)best_g;
  auto best_gi_f = 0.0;
  if (!focus_intercept(lens, best_gi, fz, best_gi_f)) {
    result.error = "focus trace blocked on rounding";
    return result;
  }
  auto best_err = std::abs(best_gi_f);
  for (auto dir : {-1, 1}) {
    auto candidate = best_g;
    for (auto step = 0; step < 4; step++) {
      candidate = std::nextafter(
          candidate, dir < 0 ? -std::numeric_limits<float>::infinity()
                             : std::numeric_limits<float>::infinity());
      auto cg = (double)candidate;
      auto cf = 0.0;
      if (!focus_intercept(lens, cg, fz, cf)) continue;
      if (std::abs(cf) < best_err) {
        best_err  = std::abs(cf);
        best_g    = candidate;
      }
    }
  }
  if (best_err > intercept_tol) {
    result.error = "focus root not representable";
    return result;
  }
  result.ok            = true;
  result.film_distance = best_g;
  return result;
}

// -----------------------------------------------------------------------------
// CAMERA INTEGRATION
// -----------------------------------------------------------------------------
bool camera_has_lens(const camera_data& camera) {
  return !camera_lens_config(camera).interfaces.empty();
}

const lens_config& camera_lens_config(const camera_data& camera) {
  return camera.lens_group;
}

lens_config& camera_lens_config(camera_data& camera) {
  return camera.lens_group;
}

bool validate_camera_lens(const camera_data& camera, string& error) {
  if (!camera_has_lens(camera)) return true;
  if (camera.orthographic) {
    error = "compound lens requires a perspective camera";
    return false;
  }
  if (!std::isfinite(camera.film) || !(camera.film > 0)) {
    error = "invalid camera film size";
    return false;
  }
  if (!std::isfinite(camera.aspect) || !(camera.aspect > 0)) {
    error = "invalid camera aspect";
    return false;
  }
  return validate_lens(camera_lens_config(camera), error);
}

bool validate_camera_lens(const camera_data& camera) {
  auto error = string{};
  return validate_camera_lens(camera, error);
}

void set_camera_lens(camera_data& camera, const lens_config& lens) {
  camera.lens_group = lens;  // deep copy owns the interface storage
}

camera_lens_sample sample_camera_lens(const camera_data& camera,
    const vec2f& image_uv, const vec2f& aperture_uv) {
  auto result = camera_lens_sample{};
  auto error  = string{};
  if (!camera_has_lens(camera) || !validate_camera_lens(camera, error)) {
    return result;
  }
  auto& lens = camera_lens_config(camera);

  // Film point in lens-local coordinates. The film occupies the same physical
  // area as the legacy camera: film by film/aspect (or the transposed size
  // for portrait aspect ratios).
  auto film = camera.aspect >= 1
                  ? vec2f{camera.film, camera.film / camera.aspect}
                  : vec2f{camera.film * camera.aspect, camera.film};
  auto fx = (double)film.x * (0.5 - (double)image_uv.x);
  auto fy = (double)film.y * ((double)image_uv.y - 0.5);
  auto g  = (double)lens.film_distance;

  auto film_point = dvec3{fx, fy, -g};
  // The previous camera jitter is already encoded in image_uv. Here we sample
  // the aperture disk uniformly by area, using aperture_uv as a unit-disk
  // sample.
  auto aperture_point = dvec3{(double)lens.aperture * (double)aperture_uv.x,
      (double)lens.aperture * (double)aperture_uv.y, 0};

  auto to_aperture = aperture_point - film_point;
  auto initial_len = length(to_aperture);
  if (!(initial_len > 0)) return result;
  auto cos_theta = to_aperture.z / initial_len;
  // The initial direction points toward the scene (+z), so cos_theta > 0.
  if (!(cos_theta > 0)) return result;

  auto traced = trace_lens_d(lens, film_point, to_aperture);
  if (!traced.valid) return result;
  // Exit rays must travel toward the scene.
  if (!(traced.direction.z > 0)) return result;

  auto geometric = (double)pi * (double)lens.aperture * (double)lens.aperture *
                   std::pow(cos_theta, 4) / (g * g);
  auto weight = geometric * traced.transmittance;
  if (!(weight > 0) || !std::isfinite(weight)) return result;

  // Map lens-local +z to camera.frame -z.
  auto local_origin    = to_vec(traced.origin);
  auto local_direction = to_vec(traced.direction);
  auto world_origin =
      transform_point(camera.frame, {local_origin.x, local_origin.y,
                                        -local_origin.z});
  auto world_direction =
      transform_direction(camera.frame, {local_direction.x, local_direction.y,
                                            -local_direction.z});

  result.ray    = {world_origin, world_direction};
  result.weight = (float)weight;
  result.valid  = true;
  return result;
}

bool focus_camera(camera_data& camera, float focus_z, float film_distance_min,
    float film_distance_max, string* error) {
  auto solved = solve_lens_focus(camera_lens_config(camera), focus_z,
      film_distance_min, film_distance_max);
  if (!solved.ok) {
    if (error) *error = solved.error;
    return false;
  }
  // Modify only after the solve succeeded, so a failure leaves the camera
  // configuration untouched.
  camera_lens_config(camera).film_distance = solved.film_distance;
  if (error) error->clear();
  return true;
}

}  // namespace yocto
