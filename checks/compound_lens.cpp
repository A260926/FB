//
// Checks for the compound lens camera.
//
// The tests use TASK_CHECK from task_support/check.h and export the
// task_cases / task_case_count tables. All optics, focus and film checks
// compare the production lens implementation against independent reference
// computations written directly in this file (sphere intersection, planar
// Snell/Fresnel, intercept tracing).
//

#include "../task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "yocto_bvh.h"
#include "yocto_lens.h"
#include "yocto_scene.h"
#include "yocto_trace.h"

using namespace yocto;

namespace {

// -----------------------------------------------------------------------------
// INDEPENDENT REFERENCE MATH
// -----------------------------------------------------------------------------

struct d3 {
  double x = 0, y = 0, z = 0;
};

inline d3 operator+(const d3& a, const d3& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline d3 operator-(const d3& a, const d3& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline d3 operator*(const d3& a, double b) { return {a.x * b, a.y * b, a.z * b}; }
inline double dot3(const d3& a, const d3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline d3 cross3(const d3& a, const d3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
      a.x * b.y - a.y * b.x};
}
inline double len3(const d3& a) { return std::sqrt(dot3(a, a)); }
inline d3 norm3(const d3& a) {
  auto l = len3(a);
  return l > 0 ? a * (1.0 / l) : d3{0, 0, 1};
}

// Independent nearest-positive sphere intersection with half-sphere and clear
// radius checks. Returns true and the point on success.
bool ref_sphere(const d3& o, const d3& d, double z, double R, double r, d3& hit) {
  auto c     = d3{0, 0, z + R};
  auto rad   = std::abs(R);
  auto oc    = o - c;
  auto b     = 2 * dot3(oc, d);
  auto cc    = dot3(oc, oc) - rad * rad;
  auto disc  = b * b - 4 * cc;
  if (disc < 0) return false;
  auto sq = std::sqrt(disc);
  auto t0 = (-b - sq) / 2, t1 = (-b + sq) / 2;
  if (t0 > t1) std::swap(t0, t1);
  auto best = 1e300;
  for (auto t : {t0, t1}) {
    if (t < -1e-9) continue;
    auto p = o + d * t;
    if ((p.z - c.z) * R >= 0) continue;
    if (p.x * p.x + p.y * p.y > r * r) continue;
    if (t < best) {
      best = t;
      hit  = p;
    }
  }
  return best < 1e299;
}

// Independent planar interface intersection.
bool ref_plane(const d3& o, const d3& d, double z, double r, d3& hit) {
  if (std::abs(d.z) < 1e-15) return false;
  auto t = (z - o.z) / d.z;
  if (t < -1e-9) return false;
  auto p = o + d * t;
  if (p.x * p.x + p.y * p.y > r * r) return false;
  hit = p;
  return true;
}

// Independent unpolarized Fresnel transmittance.
double ref_fresnel(double n1, double n2, double cosi, double cost) {
  auto rs = (n1 * cosi - n2 * cost) / (n1 * cosi + n2 * cost);
  auto rp = (n1 * cost - n2 * cosi) / (n1 * cost + n2 * cosi);
  return 1 - 0.5 * (rs * rs + rp * rp);
}

// Independent forward trace that records exit origin, direction and
// transmittance. Only camera-direction rays (toward +z) are supported.
struct ref_trace {
  bool  valid = false;
  d3    o     = {0, 0, 0};
  d3    d     = {0, 0, 1};
  double T    = 1;
};

ref_trace ref_trace_lens(const lens_config& lens, d3 o, d3 d) {
  ref_trace out;
  d          = norm3(d);
  auto count = (int)lens.interfaces.size();
  // aperture plane first
  if (!ref_plane(o, d, 0, lens.aperture, out.o)) return out;
  o = out.o;
  auto eta = 1.0;
  for (auto i = 0; i < count; i++) {
    auto& s = lens.interfaces[i];
    d3    p;
    if (s.R == 0) {
      if (!ref_plane(o, d, s.z, s.r, p)) return out;
    } else {
      if (!ref_sphere(o, d, s.z, s.R, s.r, p)) return out;
    }
    if (s.R != 0) {
      auto c   = d3{0, 0, s.z + (double)s.R};
      auto nrm = norm3(p - c);
      if (dot3(nrm, d) > 0) nrm = nrm * -1.0;
      auto cosi = -dot3(nrm, d);
      auto eta1 = eta;
      auto eta2 = (double)s.eta;
      auto ratio = eta1 / eta2;
      auto k     = 1 - ratio * ratio * (1 - cosi * cosi);
      if (k < 0) return out;
      auto cost = std::sqrt(k);
      d         = norm3(d * ratio + nrm * (ratio * cosi - cost));
      out.T *= ref_fresnel(eta1, eta2, cosi, cost);
      eta = eta2;
    }
    o = p;
  }
  out.valid = true;
  out.o     = o;
  out.d     = d;
  return out;
}

// Intercept of the traced ray at z = plane, no production code used.
bool ref_intercept(const lens_config& lens, double film_distance, double plane,
    double& x) {
  auto h  = (double)lens.aperture * 1e-3;
  auto o  = d3{0, 0, -film_distance};
  auto t  = ref_trace_lens(lens, o, d3{h, 0, 0} - o);
  if (!t.valid) return false;
  if (std::abs(t.d.z) < 1e-15) return false;
  auto tt = (plane - t.o.z) / t.d.z;
  if (!(tt > 0)) return false;
  x = t.o.x + t.d.x * tt;
  return std::isfinite(x);
}

// -----------------------------------------------------------------------------
// CONFIGURATIONS
// -----------------------------------------------------------------------------

lens_config make_biconvex(float aperture = 0.01f, float g = 0.05f) {
  auto lens  = lens_config{};
  lens.aperture      = aperture;
  lens.film_distance = g;
  lens.interfaces.push_back({0.008f, 0.015f, 0.012f, 1.5f});
  lens.interfaces.push_back({0.020f, -0.015f, 0.012f, 1.0f});
  return lens;
}

// A three-interface lens with a flat diaphragm in the middle and a rear air
// gap, exercising plane surfaces plus two curved ones.
lens_config make_triplet(float aperture = 0.008f, float g = 0.06f) {
  auto lens  = lens_config{};
  lens.aperture      = aperture;
  lens.film_distance = g;
  lens.interfaces.push_back({0.010f, 0.020f, 0.011f, 1.6f});
  lens.interfaces.push_back({0.016f, 0.0f, 0.009f, 1.6f});  // flat diaphragm
  lens.interfaces.push_back({0.028f, -0.018f, 0.011f, 1.0f});
  return lens;
}

scene_data make_scene(const lens_config& lens, float emission = 4.0f) {
  auto scene  = scene_data{};
  scene.cameras.push_back({});
  auto& camera = scene.cameras[0];
  camera.aspect = 1;
  camera.film   = 0.036f;
  set_camera_lens(camera, lens);

  auto material  = material_data{};
  material.emission = {emission, emission, emission};
  scene.materials.push_back(material);
  auto shape      = shape_data{};
  shape.positions = {
      {-10, -10, 0}, {10, -10, 0}, {10, 10, 0}, {-10, 10, 0}};
  shape.quads = {{0, 1, 2, 3}};
  scene.shapes.push_back(shape);
  auto instance    = instance_data{};
  instance.shape   = 0;
  instance.material = 0;
  instance.frame.o = {0, 0, -1.0f};
  scene.instances.push_back(instance);
  return scene;
}

trace_params make_params(int samples, trace_sampler_type sampler) {
  auto params     = trace_params{};
  params.resolution = 8;
  params.samples    = samples;
  params.bounces    = 2;
  params.sampler    = sampler;
  params.clamp      = 1e30f;
  params.seed       = 17;
  params.noparallel = true;
  return params;
}

// Renders and returns state plus the mean of the diagnostics.
struct render_result {
  trace_state state;
  double      mean_weight = 0;
  double      mean_valid  = 0;
  double      mean_image  = 0;
};

render_result render(const scene_data& scene, const trace_params& params) {
  auto result = render_result{};
  auto bvh    = make_trace_bvh(scene, params);
  auto lights = make_trace_lights(scene, params);
  result.state = make_trace_state(scene, params);
  for (auto i = 0; i < params.samples; i++)
    trace_samples(result.state, scene, bvh, lights, params);
  auto weight = get_weight_image(result.state);
  auto valid  = get_valid_image(result.state);
  auto n      = (int)result.state.width * result.state.height;
  for (auto i = 0; i < n; i++) {
    result.mean_weight += weight.pixels[i].x;
    result.mean_valid += valid.pixels[i].x;
    result.mean_image += result.state.image[i].x;
  }
  result.mean_weight /= n;
  result.mean_valid /= n;
  result.mean_image /= n;
  return result;
}

// -----------------------------------------------------------------------------
// OPTICS
// -----------------------------------------------------------------------------

int test_sphere_intersection_matches_reference() {
  auto lens = make_biconvex();
  // Check a bundle of rays against the independent sphere/plane intersection.
  for (auto ix = -4; ix <= 4; ix++) {
    for (auto iy = -3; iy <= 3; iy++) {
      auto x = (double)ix * 9e-4;
      auto y = (double)iy * 9e-4;
      auto o = d3{x, y, -(double)lens.film_distance};
      auto d = d3{0, 0, 0} - o;
      auto r = trace_lens_ray(lens, ray3f{{(float)x, (float)y,
                                             -(float)lens.film_distance},
                                   {(float)-x, (float)-y,
                                       (float)lens.film_distance}});
      auto ref = ref_trace_lens(lens, o, d);
      if (!ref.valid) {
        TASK_CHECK(!r.valid);
        continue;
      }
      TASK_CHECK(r.valid);
      TASK_CHECK(std::abs(r.origin.x - ref.o.x) < 2e-6f);
      TASK_CHECK(std::abs(r.origin.y - ref.o.y) < 2e-6f);
      TASK_CHECK(std::abs(r.origin.z - ref.o.z) < 2e-6f);
      TASK_CHECK(std::abs(r.direction.x - ref.d.x) < 2e-5f);
      TASK_CHECK(std::abs(r.direction.y - ref.d.y) < 2e-5f);
      TASK_CHECK(std::abs(r.direction.z - ref.d.z) < 2e-5f);
      TASK_CHECK(std::abs(r.transmittance - ref.T) < 1e-4f);
    }
  }
  return 0;
}

int test_plane_diaphragm_snell() {
  auto lens = make_triplet();
  // A ray that hits the flat diaphragm must not change direction there.
  auto o = d3{0.001, 0.0005, 0.0};
  auto r        = trace_lens_ray(
      lens, ray3f{{(float)o.x, (float)o.y, 0}, {0, 0, 1}});
  auto ref = ref_trace_lens(lens, o, d3{0, 0, 1});
  TASK_CHECK(ref.valid);
  TASK_CHECK(r.valid);
  TASK_CHECK(std::abs(r.transmittance - ref.T) < 1e-4f);
  // The independent and production exit directions agree, which can only
  // happen if the flat surface did not refract.
  TASK_CHECK(std::abs(r.direction.x - ref.d.x) < 2e-5f);
  TASK_CHECK(std::abs(r.direction.z - ref.d.z) < 2e-5f);
  return 0;
}

int test_transmittance_bounds() {
  auto lens = make_biconvex();
  for (auto a = -3; a <= 3; a++) {
    auto x = (double)a * 1e-3;
    auto r = trace_lens_ray(
        lens, ray3f{{(float)x, 0, -(float)lens.film_distance},
                       {(float)-x, 0, (float)lens.film_distance}});
    if (!r.valid) continue;
    TASK_CHECK(r.transmittance > 0);
    TASK_CHECK(r.transmittance <= 1.0f);
    TASK_CHECK(std::isfinite(r.transmittance));
  }
  return 0;
}

int test_aperture_blocks() {
  auto lens = make_biconvex(0.001f);
  // A ray parallel to the axis at x = 0.01 misses the 0.001-radius aperture.
  auto r = trace_lens_ray(lens,
      ray3f{{0.01f, 0.0f, -(float)lens.film_distance}, {0, 0, 1}});
  TASK_CHECK(!r.valid);
  TASK_CHECK(r.transmittance == 0);
  // A ray inside the aperture is valid.
  auto ok = trace_lens_ray(lens,
      ray3f{{0.0005f, 0.0f, -(float)lens.film_distance}, {0, 0, 1}});
  TASK_CHECK(ok.valid);
  return 0;
}

int test_total_internal_reflection() {
  // A strongly curved high-index pair of surfaces causes TIR for rays far from
  // the axis. Compare production and reference on a bundle that exercises both
  // valid and blocked rays.
  auto lens = lens_config{};
  lens.aperture = 0.02f;
  lens.film_distance = 0.05f;
  lens.interfaces.push_back({0.008f, 0.05f, 0.009f, 2.4f});
  lens.interfaces.push_back({0.016f, -0.003f, 0.0028f, 1.0f});
  TASK_CHECK(validate_lens(lens));
  auto blocked = 0;
  for (auto a = -40; a <= 40; a++) {
    auto x = (double)a * 2e-4;
    auto o = d3{0, 0, -(double)lens.film_distance};
    auto d = d3{x, 0, 0} - o;
    auto ref = ref_trace_lens(lens, o, d);
    auto r   = trace_lens_ray(lens,
          ray3f{{0, 0, -(float)lens.film_distance},
              {(float)x, 0, (float)lens.film_distance}});
    TASK_CHECK(ref.valid == r.valid);
    if (!ref.valid) blocked++;
  }
  // The configuration must actually exercise the blocking path.
  TASK_CHECK(blocked > 0);
  return 0;
}

int test_reversible_trace() {
  auto lens = make_biconvex();
  for (auto a = -3; a <= 3; a++) {
    auto x = (double)a * 8e-4;
    auto y = (double)a * 4e-4;
    auto fwd = trace_lens_ray(lens,
        ray3f{{(float)x, (float)y, -(float)lens.film_distance},
            {(float)-x, (float)-y, (float)lens.film_distance}});
    if (!fwd.valid) continue;
    auto back = trace_lens_ray(lens, ray3f{fwd.origin, -fwd.direction});
    TASK_CHECK(back.valid);
    if (!back.valid) continue;
    // Reversing must return to the film point and preserve transmittance.
    auto dz = -(double)back.direction.z;
    TASK_CHECK(dz > 0);
    auto t  = (-(double)lens.film_distance - back.origin.z) / back.direction.z;
    auto ex = back.origin.x + back.direction.x * t;
    auto ey = back.origin.y + back.direction.y * t;
    TASK_CHECK(std::abs(ex - x) < 5e-6);
    TASK_CHECK(std::abs(ey - y) < 5e-6);
    TASK_CHECK(std::abs(back.transmittance - fwd.transmittance) < 2e-4f);
  }
  return 0;
}

int test_axis_symmetry() {
  auto lens = make_biconvex();
  for (auto angle : {0.3, 1.1, 2.4, 4.0}) {
    auto c = std::cos(angle), s = std::sin(angle);
    auto x = 3e-3 * c, y = 3e-3 * s;
    auto r = trace_lens_ray(lens,
        ray3f{{(float)x, (float)y, -(float)lens.film_distance},
            {(float)-x, (float)-y, (float)lens.film_distance}});
    if (!r.valid) continue;
    // Rotate the result back to the x axis.
    auto rx = c * r.origin.x + s * r.origin.y;
    auto ry = -s * r.origin.x + c * r.origin.y;
    auto dx = c * r.direction.x + s * r.direction.y;
    auto dy = -s * r.direction.x + c * r.direction.y;
    TASK_CHECK(std::abs(ry) < 2e-6f);
    TASK_CHECK(std::abs(dy) < 2e-5f);
    // The x components must be identical for every rotation angle.
    auto ref = trace_lens_ray(lens,
        ray3f{{3e-3f, 0, -(float)lens.film_distance},
            {-3e-3f, 0, (float)lens.film_distance}});
    TASK_CHECK(std::abs(rx - ref.origin.x) < 2e-6f);
    TASK_CHECK(std::abs(dx - ref.direction.x) < 2e-5f);
    TASK_CHECK(std::abs(r.transmittance - ref.transmittance) < 1e-4f);
  }
  return 0;
}

int test_validation_rejects_bad_configs() {
  auto good = make_biconvex();
  TASK_CHECK(validate_lens(good));

  auto bad = good;
  bad.interfaces[0].z = bad.interfaces[1].z;  // not strictly increasing
  TASK_CHECK(!validate_lens(bad));

  bad = good;
  bad.interfaces[0].eta = 0;  // non-positive index
  TASK_CHECK(!validate_lens(bad));

  bad = good;
  bad.interfaces[1].eta = 1.4f;  // does not return to air
  TASK_CHECK(!validate_lens(bad));

  bad = good;
  bad.interfaces[0].r = 0.02f;  // r >= |R|
  TASK_CHECK(!validate_lens(bad));

  bad = good;
  bad.interfaces[0].R = 0;  // flat interface with a medium change
  TASK_CHECK(!validate_lens(bad));

  bad = good;
  bad.aperture = 0;  // invalid size
  TASK_CHECK(!validate_lens(bad));

  bad = good;
  bad.interfaces[0].z = std::nanf("");  // non-finite
  TASK_CHECK(!validate_lens(bad));

  bad = good;
  bad.film_distance = -1;  // invalid film distance
  TASK_CHECK(!validate_lens(bad));

  // A flat diaphragm with matching index is legal.
  auto flat = good;
  flat.interfaces.insert(flat.interfaces.begin() + 1,
      lens_interface{0.014f, 0, 0.010f, 1.5f});
  TASK_CHECK(validate_lens(flat));

  return 0;
}

}  // namespace

// -----------------------------------------------------------------------------
// OPTICS CASES
// -----------------------------------------------------------------------------
extern "C" int optics_sphere_intersection() {
  return test_sphere_intersection_matches_reference();
}
extern "C" int optics_plane_diaphragm() { return test_plane_diaphragm_snell(); }
extern "C" int optics_transmittance() { return test_transmittance_bounds(); }
extern "C" int optics_aperture_block() { return test_aperture_blocks(); }
extern "C" int optics_tir() { return test_total_internal_reflection(); }
extern "C" int optics_reversible() { return test_reversible_trace(); }
extern "C" int optics_symmetry() { return test_axis_symmetry(); }
extern "C" int optics_validation() { return test_validation_rejects_bad_configs(); }

// -----------------------------------------------------------------------------
// FOCUS CASES
// -----------------------------------------------------------------------------
namespace {

int test_focus_matches_reference() {
  auto lens = make_biconvex();
  for (auto focus_z : {0.08f, 0.12f, 0.2f, 0.4f, 0.8f}) {
    auto solved = solve_lens_focus(lens, focus_z, 0.005f, 0.3f);
    TASK_CHECK(solved.ok);
    if (!solved.ok) continue;
    // Independent trace must put the intercept within 1e-8 m of the axis.
    auto x = 0.0;
    TASK_CHECK(ref_intercept(lens, solved.film_distance, focus_z, x));
    TASK_CHECK(std::abs(x) <= 1e-8);
    // Production trace must agree with the independent one too.
    auto lc = lens;
    lc.film_distance = solved.film_distance;
    auto r = trace_lens_ray(lc,
        ray3f{{0, 0, -lc.film_distance},
            {lc.aperture * 1e-3f, 0, lc.film_distance}});
    TASK_CHECK(r.valid);
    auto tt = (focus_z - r.origin.z) / r.direction.z;
    auto px = r.origin.x + r.direction.x * tt;
    TASK_CHECK(std::abs(px) <= 1e-8f);
  }
  return 0;
}

int test_focus_endpoint_roots() {
  auto lens   = make_biconvex();
  auto solved = solve_lens_focus(lens, 0.2f, 0.005f, 0.3f);
  TASK_CHECK(solved.ok);
  if (!solved.ok) return 0;
  auto g = solved.film_distance;
  // Endpoint exactly at the root in both positions.
  auto low = solve_lens_focus(lens, 0.2f, g, g + 0.05f);
  TASK_CHECK(low.ok);
  TASK_CHECK(std::abs(low.film_distance - g) <= 1e-8f);
  auto high = solve_lens_focus(lens, 0.2f, 0.005f, g);
  TASK_CHECK(high.ok);
  TASK_CHECK(std::abs(high.film_distance - g) <= 1e-8f);
  return 0;
}

int test_focus_no_root_fails() {
  auto lens = make_biconvex();
  // Far from the focus range there is no root in a narrow interval.
  auto solved = solve_lens_focus(lens, 0.2f, 0.1f, 0.3f);
  TASK_CHECK(!solved.ok);
  TASK_CHECK(!solved.error.empty());
  return 0;
}

int test_focus_preserves_camera_on_failure() {
  auto scene   = scene_data{};
  scene.cameras.push_back({});
  auto& camera = scene.cameras[0];
  camera.film  = 0.036f;
  camera.aspect = 1.5f;
  camera.lens  = 0.05f;
  camera.focus = 123;
  auto lens    = make_biconvex();
  set_camera_lens(camera, lens);

  auto before = camera;
  auto error  = string{};
  // No root in this interval, so focusing must fail and leave the camera
  // configuration untouched.
  auto ok = focus_camera(camera, 0.2f, 0.1f, 0.3f, &error);
  TASK_CHECK(!ok);
  TASK_CHECK(!error.empty());
  TASK_CHECK(camera.film == before.film);
  TASK_CHECK(camera.focus == before.focus);
  TASK_CHECK(camera.lens == before.lens);
  TASK_CHECK(camera.lens_group.interfaces.size() ==
             before.lens_group.interfaces.size());
  TASK_CHECK(camera.lens_group.film_distance ==
             before.lens_group.film_distance);

  // Successful focusing updates only the film distance.
  auto ok2 = focus_camera(camera, 0.2f, 0.005f, 0.3f, &error);
  TASK_CHECK(ok2);
  TASK_CHECK(camera.lens_group.film_distance !=
             before.lens_group.film_distance);
  TASK_CHECK(camera.film == before.film);
  return 0;
}

int test_focus_invalid_inputs() {
  auto lens = make_biconvex();
  // focus plane behind or at the last interface is invalid
  TASK_CHECK(!solve_lens_focus(lens, 0.02f, 0.005f, 0.3f).ok);
  TASK_CHECK(!solve_lens_focus(lens, std::nanf(""), 0.005f, 0.3f).ok);
  // invalid interval
  TASK_CHECK(!solve_lens_focus(lens, 0.2f, 0.3f, 0.1f).ok);
  TASK_CHECK(!solve_lens_focus(lens, 0.2f, -0.005f, 0.3f).ok);
  // no lens configured
  auto empty = lens_config{};
  TASK_CHECK(!solve_lens_focus(empty, 0.2f, 0.005f, 0.3f).ok);
  return 0;
}

int test_focus_uses_ray_trace_not_thin_lens() {
  // A two-element lens with a rear air gap has an effective focal length that
  // differs from a single thin lens. The solved film distance for a far focus
  // must match the independent trace rather than a thin-lens formula.
  auto lens = make_triplet();
  auto solved = solve_lens_focus(lens, 0.15f, 0.005f, 0.4f);
  TASK_CHECK(solved.ok);
  if (!solved.ok) return 0;
  auto x = 0.0;
  TASK_CHECK(ref_intercept(lens, solved.film_distance, 0.15f, x));
  TASK_CHECK(std::abs(x) <= 1e-8);
  return 0;
}

}  // namespace

extern "C" int focus_matches_reference() { return test_focus_matches_reference(); }
extern "C" int focus_endpoints() { return test_focus_endpoint_roots(); }
extern "C" int focus_no_root() { return test_focus_no_root_fails(); }
extern "C" int focus_preserves() { return test_focus_preserves_camera_on_failure(); }
extern "C" int focus_invalid() { return test_focus_invalid_inputs(); }
extern "C" int focus_not_thin() { return test_focus_uses_ray_trace_not_thin_lens(); }

// -----------------------------------------------------------------------------
// FILM CASES
// -----------------------------------------------------------------------------
namespace {

int test_film_legacy_unchanged() {
  // With no lens group the camera must use the legacy thin-lens model. Check
  // that eval_camera still matches an independent implementation of the
  // documented formula, and that the camera reports no compound lens.
  auto camera     = camera_data{};
  camera.aspect   = 1.5f;
  camera.film     = 0.036f;
  camera.lens     = 0.05f;
  camera.focus    = 2.0f;
  camera.aperture = 0.01f;
  camera.frame    = rotation_frame(vec3f{0.2f, 1, 0.3f}, 0.5f);
  camera.frame.o  = {1, 2, -3};
  TASK_CHECK(!camera_has_lens(camera));

  auto film = vec2f{camera.film, camera.film / camera.aspect};
  for (auto iu = 0; iu < 5; iu++) {
    for (auto ju = 0; ju < 5; ju++) {
      auto image_uv = vec2f{(iu + 0.3f) / 5, (ju + 0.6f) / 5};
      auto lens_uv  = vec2f{0.4f * std::cos(0.7f * iu),
          0.4f * std::sin(0.7f * ju)};
      auto ray      = eval_camera(camera, image_uv, lens_uv);

      // Independent legacy thin-lens construction, in camera-local space.
      auto q  = vec3f{film.x * (0.5f - image_uv.x),
          film.y * (image_uv.y - 0.5f), camera.lens};
      auto dc = -normalize(q);
      auto e  = vec3f{lens_uv.x * camera.aperture / 2,
          lens_uv.y * camera.aperture / 2, 0};
      auto p  = dc * camera.focus / std::abs(dc.z);
      auto d  = normalize(p - e);
      auto eo = transform_point(camera.frame, e);
      auto ed = transform_direction(camera.frame, d);
      TASK_CHECK(std::abs(ray.o.x - eo.x) < 1e-6f);
      TASK_CHECK(std::abs(ray.o.y - eo.y) < 1e-6f);
      TASK_CHECK(std::abs(ray.o.z - eo.z) < 1e-6f);
      TASK_CHECK(std::abs(ray.d.x - ed.x) < 1e-5f);
      TASK_CHECK(std::abs(ray.d.y - ed.y) < 1e-5f);
      TASK_CHECK(std::abs(ray.d.z - ed.z) < 1e-5f);
    }
  }

  // A legacy scene renders to a finite, non-trivial image and accumulates no
  // compound lens diagnostics.
  auto scene   = make_scene(lens_config{}, 4.0f);
  auto params  = make_params(4, trace_sampler_type::path);
  auto result  = render(scene, params);
  auto any     = false;
  for (auto& pixel : result.state.image) {
    TASK_CHECK(std::isfinite(pixel.x));
    if (pixel.x > 0) any = true;
  }
  TASK_CHECK(any);
  auto all_zero = true;
  for (auto value : result.state.weight_sum)
    if (value != 0) all_zero = false;
  TASK_CHECK(all_zero);
  return 0;
}

int test_film_irradiance_scales_with_area() {
  // In a constant environment the irradiance must scale with aperture area,
  // for both path and naive samplers.
  for (auto sampler : {trace_sampler_type::path, trace_sampler_type::naive}) {
    auto params = make_params(96, sampler);
    auto small  = render(make_scene(make_biconvex(0.001f)), params);
    auto large  = render(make_scene(make_biconvex(0.002f)), params);
    // 4x area must give roughly 4x irradiance.
    auto ratio = large.mean_image / small.mean_image;
    TASK_CHECK(ratio > 3.5);
    TASK_CHECK(ratio < 4.5);
    // The weight diagnostic also scales with area.
    auto wratio = large.mean_weight / small.mean_weight;
    TASK_CHECK(wratio > 3.5);
    TASK_CHECK(wratio < 4.5);
  }
  return 0;
}

int test_film_failed_samples_keep_denominator() {
  // Use a wide aperture but a small interface clear radius, so many camera
  // samples pass the aperture yet miss the first interface. Valid samples must
  // not be renormalized away: the diagnostics divide by the full camera sample
  // count.
  auto lens = lens_config{};
  lens.aperture      = 0.008f;
  lens.film_distance = 0.05f;
  lens.interfaces.push_back({0.010f, 0.020f, 0.004f, 1.5f});
  lens.interfaces.push_back({0.020f, -0.020f, 0.004f, 1.0f});
  TASK_CHECK(validate_lens(lens));
  auto scene  = make_scene(lens, 4.0f);
  auto params = make_params(24, trace_sampler_type::path);
  auto result = render(scene, params);

  // Independently estimate the valid fraction by sampling film points and the
  // aperture disk on a dense deterministic grid with the reference tracer.
  auto valid = 0, total = 0;
  for (auto j = 0; j < 40; j++) {
    for (auto i = 0; i < 40; i++) {
      auto u    = (i + 0.5) / 40.0;
      auto v    = (j + 0.5) / 40.0;
      auto film = vec2f{0.036f * (float)(0.5 - u), 0.036f * (float)(v - 0.5)};
      for (auto k = 0; k < 40; k++) {
        auto rr  = (k + 0.5) / 40.0;
        auto phi = 2 * pi * ((k * 0.618034) - std::floor(k * 0.618034));
        auto ar  = lens.aperture * std::sqrt(rr);
        auto apx = ar * std::cos(phi);
        auto apy = ar * std::sin(phi);
        auto t   = ref_trace_lens(lens,
            d3{film.x, film.y, -lens.film_distance},
            d3{apx - film.x, apy - film.y, lens.film_distance});
        total++;
        if (t.valid) valid++;
      }
    }
  }
  auto ref_fraction = (double)valid / total;
  TASK_CHECK(ref_fraction > 0.05);
  TASK_CHECK(ref_fraction < 0.95);
  // The diagnostic must match the reference fraction.
  TASK_CHECK(std::abs(result.mean_valid - ref_fraction) < 0.05);
  // Successes-only averaging would give a much larger mean valid fraction
  // (approaching one); the full-denominator value must stay well below one.
  TASK_CHECK(result.mean_valid < 0.9);
  // The weight mean includes zeros for blocked samples. If it averaged over
  // successes only (the value would then be roughly mean_weight / valid), it
  // would be about 7x larger here. Check that it matches the full-denominator
  // expectation within a small factor instead.
  TASK_CHECK(result.mean_weight > 0);
  TASK_CHECK(result.mean_weight < 0.05);
  TASK_CHECK(result.mean_weight / result.mean_valid < 0.5);
  return 0;
}

int test_film_weight_matches_analytic() {
  // For a single flat diaphragm lens (no refraction) the weight reduces to the
  // analytic geometry factor with unit Fresnel transmittance.
  auto lens = lens_config{};
  lens.aperture      = 0.004f;
  lens.film_distance = 0.05f;
  // No curved interfaces; a single flat diaphragm at z = 0.01 that does not
  // change the medium. The ray must still exit.
  lens.interfaces.push_back({0.01f, 0, 0.01f, 1.0f});
  TASK_CHECK(validate_lens(lens));

  auto scene   = scene_data{};
  scene.cameras.push_back({});
  auto& camera = scene.cameras[0];
  camera.aspect = 1;
  camera.film   = 0.02f;
  set_camera_lens(camera, lens);

  // Sample the camera directly and compare against the analytic weight.
  auto max_err = 0.0;
  for (auto i = 0; i < 20; i++) {
    auto u   = (i + 0.5) / 20.0;
    auto v   = 0.5;
    auto rr  = ((i * 37) % 20 + 0.5) / 20.0;
    auto phi = 2 * pi * ((i * 53) % 20) / 20.0;
    auto aur = std::sqrt(rr);
    auto ap  = vec2f{(float)(aur * std::cos(phi)), (float)(aur * std::sin(phi))};
    auto s   = sample_camera_lens(camera, {(float)u, (float)v}, ap);
    // The sample may be blocked by the diaphragm clear radius; only check
    // valid samples.
    if (!s.valid) continue;
    auto fx = 0.02 * (0.5 - u);
    auto fy = 0.02 * (v - 0.5);
    auto apx = lens.aperture * ap.x;
    auto apy = lens.aperture * ap.y;
    auto dz  = lens.film_distance;
    auto l   = std::sqrt((apx - fx) * (apx - fx) + (apy - fy) * (apy - fy) +
                        dz * dz);
    auto cos4 = std::pow(dz / l, 4.0);
    auto expected =
        pi * lens.aperture * lens.aperture * cos4 / (dz * dz);
    max_err = std::max(max_err, std::abs(s.weight - expected) / expected);
  }
  TASK_CHECK(max_err < 1e-4);
  return 0;
}

int test_film_diagnostics_ignore_clamp() {
  // The weight and valid channels must be identical with aggressive clamping.
  auto scene  = make_scene(make_biconvex(0.004f), 4.0f);
  auto params_hi = make_params(16, trace_sampler_type::path);
  auto params_lo = params_hi;
  params_lo.clamp = 1e-6f;
  auto hi = render(scene, params_hi);
  auto lo = render(scene, params_lo);
  TASK_CHECK(std::abs(hi.mean_weight - lo.mean_weight) < 1e-9);
  TASK_CHECK(std::abs(hi.mean_valid - lo.mean_valid) < 1e-9);
  // The rendered image is clamped.
  TASK_CHECK(lo.mean_image < hi.mean_image);
  TASK_CHECK(lo.mean_image <= 1e-6f * 1.01);
  return 0;
}

int test_film_diagnostics_linear_float() {
  auto scene  = make_scene(make_biconvex(0.004f), 4.0f);
  auto params = make_params(8, trace_sampler_type::path);
  auto result = render(scene, params);
  auto weight = get_weight_image(result.state);
  auto valid  = get_valid_image(result.state);
  TASK_CHECK(weight.linear);
  TASK_CHECK(valid.linear);
  TASK_CHECK(weight.width == result.state.width);
  TASK_CHECK(weight.height == result.state.height);
  for (auto i = 0; i < (int)weight.pixels.size(); i++) {
    TASK_CHECK(std::isfinite(weight.pixels[i].x));
    TASK_CHECK(weight.pixels[i].x >= 0);
    TASK_CHECK(valid.pixels[i].x >= 0 && valid.pixels[i].x <= 1);
  }
  return 0;
}

int test_film_progressive_batches() {
  // Progressive rendering with multiple batches must match a single batch,
  // for both the path and naive samplers.
  for (auto sampler : {trace_sampler_type::path, trace_sampler_type::naive}) {
    auto scene  = make_scene(make_biconvex(0.004f), 4.0f);
    auto params = make_params(8, sampler);

    auto single = render(scene, params);

    auto batch = params;
    batch.batch = 2;
    auto bvh    = make_trace_bvh(scene, batch);
    auto lights = make_trace_lights(scene, batch);
    auto state  = make_trace_state(scene, batch);
    for (auto i = 0; i < batch.samples / batch.batch; i++)
      trace_samples(state, scene, bvh, lights, batch);

    TASK_CHECK(state.image.size() == single.state.image.size());
    for (auto i = 0; i < (int)state.image.size(); i++) {
      TASK_CHECK(std::abs(state.image[i].x - single.state.image[i].x) < 1e-6f);
      TASK_CHECK(std::abs(state.image[i].y - single.state.image[i].y) < 1e-6f);
    }
    // Diagnostics must also match batch to batch.
    TASK_CHECK(state.weights.size() == single.state.weights.size());
    for (auto i = 0; i < (int)state.weights.size(); i++) {
      TASK_CHECK(std::abs(state.weights[i] - single.state.weights[i]) < 1e-9f);
      TASK_CHECK(std::abs(state.valid[i] - single.state.valid[i]) < 1e-9f);
    }
  }
  return 0;
}

int test_film_rigid_transform() {
  // Rotating and translating the camera must move the sample ray by the same
  // rigid transform while preserving the weight.
  auto lens  = make_biconvex(0.004f);
  auto scene = make_scene(lens, 4.0f);
  auto& camera = scene.cameras[0];
  auto s0 = sample_camera_lens(camera, {0.37f, 0.61f}, {0.2f, -0.3f});
  TASK_CHECK(s0.valid);

  auto rotated = camera;
  rotated.frame = rotation_frame(vec3f{0, 1, 0}, 0.7f);
  rotated.frame.o = {3, -2, 5};
  auto s1 = sample_camera_lens(rotated, {0.37f, 0.61f}, {0.2f, -0.3f});
  TASK_CHECK(s1.valid);
  auto expected_o = transform_point(rotated.frame,
      transform_vector_inverse(camera.frame, s0.ray.o));
  auto expected_d = transform_direction(rotated.frame,
      transform_vector_inverse(camera.frame, s0.ray.d));
  TASK_CHECK(std::abs(s1.ray.o.x - expected_o.x) < 2e-5f);
  TASK_CHECK(std::abs(s1.ray.o.y - expected_o.y) < 2e-5f);
  TASK_CHECK(std::abs(s1.ray.o.z - expected_o.z) < 2e-5f);
  TASK_CHECK(std::abs(s1.ray.d.x - expected_d.x) < 2e-5f);
  TASK_CHECK(std::abs(s1.ray.d.y - expected_d.y) < 2e-5f);
  TASK_CHECK(std::abs(s1.ray.d.z - expected_d.z) < 2e-5f);
  TASK_CHECK(std::abs(s1.weight - s0.weight) < 1e-6f);
  return 0;
}

int test_film_owner_independent() {
  // Modifying the caller's interface array after set_camera_lens must not
  // change the camera's own copy.
  auto lens = make_biconvex(0.004f);
  auto scene   = scene_data{};
  scene.cameras.push_back({});
  auto& camera = scene.cameras[0];
  camera.aspect = 1;
  set_camera_lens(camera, lens);
  auto before = sample_camera_lens(camera, {0.3f, 0.6f}, {0.1f, 0.2f});
  TASK_CHECK(before.valid);
  // Mutate the caller array.
  lens.interfaces[0].R = 0.5f;
  lens.interfaces.clear();
  lens.aperture = 1.0f;
  auto after = sample_camera_lens(camera, {0.3f, 0.6f}, {0.1f, 0.2f});
  TASK_CHECK(after.valid);
  TASK_CHECK(std::abs(after.weight - before.weight) < 1e-12f);
  TASK_CHECK(std::abs(after.ray.d.x - before.ray.d.x) < 1e-7f);
  return 0;
}

int test_film_old_thin_lens_image() {
  // The legacy thin-lens image must be unchanged by the compound lens work.
  // Render a legacy scene, then re-create the exact legacy ray for the first
  // pixel/sample with an independent RNG and compare the traced radiance.
  auto scene   = scene_data{};
  scene.cameras.push_back({});
  auto& camera = scene.cameras[0];
  camera.aspect = 1;
  camera.film   = 0.036f;
  camera.lens   = 0.05f;
  camera.focus  = 1.0f;
  camera.aperture = 0.01f;
  TASK_CHECK(!camera_has_lens(camera));

  auto material    = material_data{};
  material.emission = {3, 3, 3};
  scene.materials.push_back(material);
  auto shape      = shape_data{};
  shape.positions = {{-4, -4, 0}, {4, -4, 0}, {4, 4, 0}, {-4, 4, 0}};
  shape.quads     = {{0, 1, 2, 3}};
  scene.shapes.push_back(shape);
  auto instance    = instance_data{};
  instance.shape   = 0;
  instance.material = 0;
  instance.frame.o = {0, 0, -1};
  scene.instances.push_back(instance);

  auto params = make_params(2, trace_sampler_type::path);
  auto bvh    = make_trace_bvh(scene, params);
  auto lights = make_trace_lights(scene, params);
  auto state  = make_trace_state(scene, params);

  // Reconstruct the first legacy sample for pixel (0,0) with a fresh RNG copy
  // and compare its camera ray against eval_camera. This pins the legacy
  // sampling before any compound lens code runs.
  auto rng = state.rngs[0];
  auto puv = rand2f(rng);
  auto luv = sample_disk(rand2f(rng));
  auto uv  = vec2f{puv.x / state.width, puv.y / state.height};
  auto ray = eval_camera(camera, uv, luv);
  auto film  = vec2f{camera.film, camera.film / camera.aspect};
  auto q     = vec3f{film.x * (0.5f - uv.x), film.y * (uv.y - 0.5f),
      camera.lens};
  auto dc    = -normalize(q);
  auto e     = vec3f{luv.x * camera.aperture / 2,
      luv.y * camera.aperture / 2, 0};
  auto p     = dc * camera.focus / std::abs(dc.z);
  auto d     = normalize(p - e);
  auto eo    = transform_point(camera.frame, e);
  auto ed    = transform_direction(camera.frame, d);
  TASK_CHECK(std::abs(ray.o.x - eo.x) < 1e-6f);
  TASK_CHECK(std::abs(ray.d.z - ed.z) < 1e-6f);

  // The image itself must be finite and positive on this constant emitter.
  trace_samples(state, scene, bvh, lights, params);
  for (auto& pixel : state.image) {
    TASK_CHECK(std::isfinite(pixel.x));
    TASK_CHECK(pixel.x >= 0);
  }
  auto any = false;
  for (auto& pixel : state.image)
    if (pixel.x > 0) any = true;
  TASK_CHECK(any);
  return 0;
}

int test_film_clamp_after_weight() {
  // Clamping must happen after multiplying by the exposure weight. With a
  // clamp C much smaller than the weighted radiance, every sample saturates at
  // C. Applying the clamp before the weight multiply would leave values near
  // C * weight, much smaller than C.
  auto scene     = make_scene(make_biconvex(0.004f), 5000.0f);
  auto params    = make_params(8, trace_sampler_type::path);
  auto unclamped = render(scene, params);
  params.clamp   = 0.5f;
  auto clamped   = render(scene, params);
  TASK_CHECK(unclamped.mean_image > 10.0f);
  // Saturated samples sit at the clamp value; the mean is bounded by it.
  TASK_CHECK(clamped.mean_image <= 0.5f * 1.001f);
  // Applying the clamp before the weight multiply would scale the image by the
  // mean weight and be far below the mean valid fraction times the clamp.
  TASK_CHECK(clamped.mean_image > 0.5f * clamped.mean_valid * 0.5);
  // Diagnostics are the unclamped values.
  TASK_CHECK(std::abs(clamped.mean_weight - unclamped.mean_weight) < 1e-9);
  TASK_CHECK(std::abs(clamped.mean_valid - unclamped.mean_valid) < 1e-9);
  return 0;
}

}  // namespace

extern "C" int film_legacy() { return test_film_legacy_unchanged(); }
extern "C" int film_clamp_after() { return test_film_clamp_after_weight(); }
extern "C" int film_irradiance() { return test_film_irradiance_scales_with_area(); }
extern "C" int film_denominator() { return test_film_failed_samples_keep_denominator(); }
extern "C" int film_weight() { return test_film_weight_matches_analytic(); }
extern "C" int film_clamp() { return test_film_diagnostics_ignore_clamp(); }
extern "C" int film_linear() { return test_film_diagnostics_linear_float(); }
extern "C" int film_batches() { return test_film_progressive_batches(); }
extern "C" int film_transform() { return test_film_rigid_transform(); }
extern "C" int film_owner() { return test_film_owner_independent(); }
extern "C" int film_thin_lens() { return test_film_old_thin_lens_image(); }

// -----------------------------------------------------------------------------
// CASE TABLE
// -----------------------------------------------------------------------------
const struct task_case task_cases[] = {
    {"optics", "sphere_intersection", optics_sphere_intersection},
    {"optics", "plane_diaphragm", optics_plane_diaphragm},
    {"optics", "transmittance", optics_transmittance},
    {"optics", "aperture_block", optics_aperture_block},
    {"optics", "total_internal_reflection", optics_tir},
    {"optics", "reversible", optics_reversible},
    {"optics", "axis_symmetry", optics_symmetry},
    {"optics", "validation", optics_validation},
    {"focus", "matches_reference", focus_matches_reference},
    {"focus", "endpoints", focus_endpoints},
    {"focus", "no_root", focus_no_root},
    {"focus", "preserves_camera", focus_preserves},
    {"focus", "invalid_inputs", focus_invalid},
    {"focus", "not_thin_lens", focus_not_thin},
    {"film", "legacy_unchanged", film_legacy},
    {"film", "irradiance_area", film_irradiance},
    {"film", "failed_denominator", film_denominator},
    {"film", "weight_analytic", film_weight},
    {"film", "clamp_isolated", film_clamp},
    {"film", "clamp_after_weight", film_clamp_after},
    {"film", "linear_float", film_linear},
    {"film", "progressive_batches", film_batches},
    {"film", "rigid_transform", film_transform},
    {"film", "owner_independent", film_owner},
    {"film", "thin_lens_image", film_thin_lens},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
