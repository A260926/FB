#include "light.hpp"
#include "sphere_connection.hpp"
#include <algorithm>
#include <limits>

namespace {

// Whether the open segment A -> B enters the strict interior of `sphere`
// (a tangent touch on the boundary does not count as entering).
bool SegmentEntersSphere(const Sphere& sphere, const Vec3f& A,
                         const Vec3f& B) {
  const Vec3f d = (B - A).Unit();
  const Vec3f f = A - sphere.center;
  const float r = sphere.radius;
  const float b = d.Dot(f);
  const float c = f.Dot(f) - r * r;
  const float disc = b * b - c; // |d| = 1, so a = 1
  if (disc <= 0.0f)
    return false; // no crossing or mere tangency: never enters the interior
  const float sq = std::sqrt(disc);
  const float t_near = -b - sq;
  const float t_far = -b + sq;
  const float len = (B - A).Norm();
  // interior is traversed for t in (t_near, t_far); overlap with the
  // open segment (0, len) means the direct path crosses the ball
  return t_far > 0.0f && t_near < len;
}

// Occlusion test on the open segment A -> B against every object except
// the lens sphere (its role is already encoded in the connections).
// `skip_receiver` also ignores the receiving surface; it is only used
// for the segment leaving the receiver to avoid self-intersection.
// The endpoint tolerance stays an order of magnitude below the 1e-2*R
// occluder clearance required by the task, so legitimate occluders near
// either endpoint are still detected, and anything past B is ignored.
bool SegmentOccluded(const std::vector<std::unique_ptr<Object>>& objects,
                     const Sphere& lens, const Object* receiver,
                     const Vec3f& A, const Vec3f& B, bool skip_receiver) {
  Ray ray(A, B);
  const float len = (B - A).Norm();
  const float tol = 1e-3f * lens.radius;
  for (const auto& obj : objects) {
    if (obj.get() == &lens)
      continue; // the lens is never treated as an opaque occluder
    if (skip_receiver && obj.get() == receiver)
      continue;
    if (!IntersectsAABB(ray, obj->bounds))
      continue;
    const HitRecord hit = obj->Intersects(ray);
    if (hit.is_hit && hit.t > tol && hit.t < len - tol)
      return true;
  }
  return false;
}

} // namespace

void Lights::AddAmbient(float intensity) {
  lights_.push_back(Light{.type = LightType::AMBIENT,
                          .intensity = intensity,
                          .data = std::nullopt});
}
void Lights::AddPoint(float intensity, float posx, float posy, float posz) {
  lights_.push_back(Light{.type = LightType::POINT,
                          .intensity = intensity,
                          .data = Vec3f{posx, posy, posz}});
}
void Lights::AddDir(float intensity, float dirx, float diry, float dirz) {
  lights_.push_back(Light{.type = LightType::DIRECTIONAL,
                          .intensity = intensity,
                          .data = Vec3f{dirx, diry, dirz}.Unit()});
}

// call it having added all lights to normalize their intensities
void Lights::Normalize() {
  float total = 0.0;
  for (const auto &light: lights_) total += light.intensity;
  if (std::abs(total) < eps) return;
  for (auto &light: lights_) light.intensity /= total;
}

// Diffuse/specular weights of one point light when a lens sphere is
// active. The unoccluded straight connection and every refracted
// sphere connection are evaluated independently; each visible path is
// shaded with the unit direction of its first segment, and the path
// weights (1 for the direct path, `transmission` otherwise) are applied.
namespace {

struct PointContribution {
  float diffuse{0.0f};
  float specular{0.0f};
};

PointContribution LensPointContribution(
    const Light& light,
    const std::vector<std::unique_ptr<Object>>& objects,
    const Object& object, const Vec3f& at, const Vec3f& N,
    const Sphere& lens, const Vec3f& view_dir, bool has_specular) {
  PointContribution acc;
  const Vec3f light_pos = *light.data;

  // ---- direct (unrefracted) connection, valid only if its open segment
  // does not enter the lens interior; other objects are opaque blockers.
  // It still has to face the receiving hemisphere, independently of the
  // refracted paths (a back-facing straight ray must never contribute).
  if (!SegmentEntersSphere(lens, at, light_pos)) {
    const Vec3f direct_dir = (light_pos - at).Unit();
    const float direct_ndotl = N.Dot(direct_dir);
    if (direct_ndotl > 0.0f &&
        !SegmentOccluded(objects, lens, &object, at, light_pos, true)) {
      if (has_specular) {
        const Vec3f reflected = (-direct_dir).ReflectAbout(N, true);
        const float refl_dot_view =
            std::max(reflected.Dot(view_dir), 0.0f);
        acc.specular += light.intensity *
                        std::pow(refl_dot_view, object.material.specular);
      }
      acc.diffuse += light.intensity * direct_ndotl;
    }
  }

  // ---- refracted air -> sphere -> air connections
  const std::vector<SphereConnection> connections =
      ConnectSphere(lens, at, light_pos);
  for (const SphereConnection& conn : connections) {
    const Vec3f first_dir = (conn.entry - at).Unit();
    const float ndotl = N.Dot(first_dir);
    if (ndotl <= 0.0f)
      continue; // this path arrives from behind the receiving hemisphere
    // the three open segments are occlusion-tested separately; the lens
    // itself is never an occluder, and the receiver is skipped only on
    // the first segment
    if (SegmentOccluded(objects, lens, &object, at, conn.entry, true))
      continue;
    if (SegmentOccluded(objects, lens, &object, conn.entry, conn.exit,
                        false))
      continue;
    if (SegmentOccluded(objects, lens, &object, conn.exit, light_pos,
                        false))
      continue;
    if (has_specular) {
      const Vec3f reflected = (-first_dir).ReflectAbout(N, true);
      const float refl_dot_view =
          std::max(reflected.Dot(view_dir), 0.0f);
      acc.specular += light.intensity *
                      std::pow(refl_dot_view, object.material.specular) *
                      static_cast<float>(conn.transmission);
    }
    acc.diffuse += light.intensity * ndotl *
                   static_cast<float>(conn.transmission);
  }
  return acc;
}

} // namespace

// diffuse and specular light contribution at a point on an object
Vec3u8 Lights::ColorAt(const std::vector<std::unique_ptr<Object>>& objects,
                       const Object &object,
                       const Vec3f &at,
                       const Camera &camera,
                       const Vec3f &normal) const {
  float diffuse_intensity = 0.0;
  float specular_intensity = 0.0;
  const Vec3f& N = normal;
  // view_dir is only needed for the specular term; avoid its sqrt for
  // matte (specular == 0) objects, which are the common case
  const bool has_specular = object.material.specular > 0;
  const Vec3f view_dir = has_specular
      ? (camera.center() - at).Unit()
      : Vec3f{};

  // lens mode applies only to opaque exterior receivers; the lens itself,
  // points inside it, and transparent surfaces keep the legacy straight
  // line path
  const Sphere* lens = point_refractor_;
  const bool lens_active =
      lens != nullptr && &object != lens &&
      object.material.transparency < eps && !lens->IsInside(at);

  for (const auto &light: lights_) {
    if (light.type == LightType::AMBIENT) {
      diffuse_intensity += light.intensity;
      continue; // ambient light isn't affected by shadows
    }

    if (light.type == LightType::POINT && lens_active) {
      const PointContribution pc = LensPointContribution(
          light, objects, object, at, N, *lens, view_dir, has_specular);
      diffuse_intensity += pc.diffuse;
      specular_intensity += pc.specular;
      continue;
    }

    /*
     * D ___\___              D: directional source
     *      \___\___          P: point source
     *          \___\___      N: normal
     *              \___\___              ^N
     *                  \____\___        /
     *                       \___\___    |
     *   _______                 \___\__/   *****
     * P __     \_____________       \__*************
     *     \___               \_______*****************
     *         \___                  *******************
     *             \__              *********************
     *                \___          *********************
     *                    \___     ***********************
     *                        \___  *********************
     *                            \_*********************
     */
    // diffuse light direction
    Vec3f light_dir = (light.type == LightType::POINT)
                      ? (*light.data - at).Unit()
                      : *light.data;
    const float ndotl = N.Dot(light_dir);
    // light behind the surface: no diffuse or specular contribution, so
    // skip the (expensive) shadow test too
    if (ndotl <= 0)
      continue;

    // check for shadows before computing diffuse/specular component
    float shadow_brightness = ShadowFactor(light, objects,
                   object, at, N);
    if (shadow_brightness < eps)
      continue; // fully occluded - save computation time

    // ref:
    // gabrielgambetta.com/computer-graphics-from-scratch/03-light.html
    if (has_specular) {
      // mirror the direction to the light about the normal; negate first as
      // ReflectAbout expects a vector pointing towards the surface
      Vec3f reflected = (-light_dir).ReflectAbout(N, true);
      float refl_dot_view = std::max(reflected.Dot(view_dir), 0.0f);
      specular_intensity += light.intensity *
                            std::pow(refl_dot_view, object.material.specular) *
                            shadow_brightness;
    }
    diffuse_intensity += light.intensity * ndotl * shadow_brightness;
  }

  diffuse_intensity = std::min(diffuse_intensity, 1.0f);
  specular_intensity = std::min(specular_intensity, 1.0f);
  //uint8_t r = sphere.material.color.x;
  //uint8_t g = sphere.material.color.y;
  //uint8_t b = sphere.material.color.z;
  Vec3u8 sample = object.SampleColor(at, N);
  uint8_t r = sample.x, g = sample.y, b = sample.z;
  return Vec3u8{
    static_cast<uint8_t>(std::min(r * diffuse_intensity +
                                  255*specular_intensity , 255.0f)),
    static_cast<uint8_t>(std::min(g * diffuse_intensity +
                                  255*specular_intensity , 255.0f)),
    static_cast<uint8_t>(std::min(b * diffuse_intensity +
                                  255*specular_intensity , 255.0f)),
  };
}

float Lights::ShadowFactor(const Light& light,
                           const std::vector<std::unique_ptr<Object>>& objects,
                           const Object& object,
                           const Vec3f& at,
                           const Vec3f& normal) {
  // origin is pushed off the surface per light type (below) to avoid
  // self-intersection, i.e. shadow acne
  Vec3f origin;
  constexpr float bright_min = 0.0, bright_max = 1.0;
  float ret = bright_max; // no shadow
  /*
   *  X: intersection                    To determine whether
   *                       dir. source   a shdaow is cast:
   *             original   /            1. reverse the direction/
   *             ray      -/                point to source and start
   *              ***** -V                  from intersection
   *            *********                2. If it hits another object
   *            *********                   AND there is no occlusion
   *            *********                   cast a shadow, i.e. do not
   *              *****                     count contribution from
   *         orig.  |^  shadow              this source
   *         ray    //  ray
   *              v/  
   *           ***X*******       
   *        *****************    
   */
  if (light.type == LightType::POINT) {
    // shadow ray is directed from intersection to light source
    Vec3f dir_to_light = (*light.data - at).Unit();
    // push origin along the normal hemisphere w.r.t. light direction
    Vec3f hemi = (normal.Dot(dir_to_light) > 0 ? normal : -normal);
    origin = at + hemi * eps * 4.0f;
    Ray shadow_ray(origin, *light.data);
    float light_dist = (*light.data - origin).Norm();
    
    float t_nearest = std::numeric_limits<float>::infinity();
    bool any_hit = false;
    for (const auto &obj : objects) {
      // skip self-intersection
      if (obj.get() == &object) continue;
      if (!IntersectsAABB(shadow_ray, obj->bounds)) continue;
      HitRecord hit = obj->Intersects(shadow_ray);
      // check if object blocks the light (0 < t < light_distance)
      // i.e. object is between surface and the point source
      if (hit.is_hit && hit.t > 0 && hit.t < light_dist) {
        any_hit = true;
        if (hit.t < t_nearest)
          t_nearest = hit.t;
      }
    }
    if (!any_hit)
      return bright_max;

    // Shadow brightness heuristic;
    // for brightness = 1 we have no shadow, for 0 it's fully dark.
    // Use (1) the distance to source 
    // and (2) the relative direction between the normal and the
    // direction to source to compute a brightness factor within [0,1]
    // dir_to_light is exactly the shadow ray's (normalized) direction,
    // so reuse it instead of normalizing again
    dir_to_light = shadow_ray.dir;
    // how perpendicular the ray is to the normal (2)
    float ndotl = normal.Dot(dir_to_light);
    // normalized distance from target to source
    float u = std::clamp(t_nearest / light_dist, 0.0f, 1.0f);
    ret = ndotl * u;
  } else if (light.type == LightType::DIRECTIONAL) {
    // push origin along the normal hemisphere w.r.t. light direction
    Vec3f hemi = (normal.Dot(*light.data) > 0 ? normal : -normal);
    origin = at + hemi * eps * 4.0f;
    Ray shadow_ray {origin, {}};
    // shadow ray has travels in the opposite direction as the light
    shadow_ray.dir = -*light.data;
    // if the shadow ray intersects another object, cast a shadow
    // for directional lights, any hit with t > 0 means shadow
    bool any_hit = std::any_of(objects.begin(), objects.end(),
                   [&](const auto& obj) {
                     if (obj.get() == &object) return false;
                     if (!IntersectsAABB(shadow_ray, obj->bounds)) return false;
                     auto hit = obj->Intersects(shadow_ray);
                     if (!hit.is_hit || hit.t <= 0) return false;
                     // normal of the other object at intersection
                     Vec3f n_other = obj->NormalAt(hit.where);
                     // occlusion - one object is inside another
                     bool occluded = normal.Dot(shadow_ray.dir) < 0 &&
                       n_other.Dot(shadow_ray.dir) < 0; 
                     if (occluded) return false;
                     return true;
                  });
    if (!any_hit)
      return bright_max;

    const Vec3f light_dir = -*light.data;
    // same as before, however use only part (2) of the heuristic
    ret = std::clamp(normal.Dot(light_dir), 0.0f, 1.0f);

  }
  return std::clamp(ret, bright_min, bright_max);
}
