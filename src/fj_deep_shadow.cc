// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#include "fj_deep_shadow.h"
#include "fj_shading.h"
#include "fj_shader.h"
#include "fj_object_group.h"
#include "fj_object_set.h"
#include "fj_object_instance.h"
#include "fj_accelerator.h"
#include "fj_primitive_set.h"
#include "fj_intersection.h"
#include "fj_numeric.h"
#include "fj_ray.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fj {

static const int CUBE_FACE_COUNT = 6;

// Hits closer than this relative tolerance share one breakpoint. True
// co-located layers (two instances of the same geometry) come out bit
// identical; this only absorbs floating-point round-off.
static Real distance_merge_epsilon(Real max_distance)
{
  return 1e-9 * Max(1., max_distance);
}

struct DeepShadowHit {
  DeepShadowHit() : distance(0), factor(1), object(NULL), prim_id(0) {}
  ~DeepShadowHit() {}

  Real distance;
  Real factor;
  const ObjectInstance *object;
  int prim_id;
};

static bool hit_distance_less(const DeepShadowHit &a, const DeepShadowHit &b)
{
  return a.distance < b.distance;
}

// Cube face direction templates from task_support/cube_contract.md.
static Vector face_direction(int face, Real s, Real t)
{
  switch (face) {
  case 0: return Vector( 1, -t, -s); // +X
  case 1: return Vector(-1, -t,  s); // -X
  case 2: return Vector( s,  1,  t); // +Y
  case 3: return Vector( s, -1, -t); // -Y
  case 4: return Vector( s, -t,  1); // +Z
  case 5: return Vector(-s, -t, -1); // -Z
  default:
    return Vector(0, 0, 1);
  }
}

DeepShadowMap::DeepShadowMap() :
    resolution_(0),
    max_distance_(0),
    time_(0),
    target_(NULL),
    built_(false)
{
}

DeepShadowMap::~DeepShadowMap()
{
}

void DeepShadowMap::Clear()
{
  resolution_ = 0;
  max_distance_ = 0;
  time_ = 0;
  target_ = NULL;
  built_ = false;
  std::vector<std::vector<DeepShadowBreakpoint> >().swap(staircases_);
}

Vector DeepShadowMap::texel_direction(int face, int x, int y, int resolution)
{
  const Real u = (x + .5) / resolution;
  const Real v = (y + .5) / resolution;
  const Real s = 2 * u - 1;
  const Real t = 2 * v - 1;
  return Normalize(face_direction(face, s, t));
}

void DeepShadowMap::direction_to_face_uv(const Vector &dir,
    int *face, Real *u, Real *v)
{
  const Real ax = Abs(dir.x);
  const Real ay = Abs(dir.y);
  const Real az = Abs(dir.z);

  int f = 0;
  Real s = 0;
  Real t = 0;

  // largest absolute component; ties resolve X before Y before Z
  if (ax >= ay && ax >= az) {
    const Real a = ax;
    if (dir.x >= 0) {
      f = 0; // +X: (1,-t,-s)
      t = -dir.y / a;
      s = -dir.z / a;
    } else {
      f = 1; // -X: (-1,-t,s)
      t = -dir.y / a;
      s =  dir.z / a;
    }
  } else if (ay >= az) {
    const Real a = ay;
    if (dir.y >= 0) {
      f = 2; // +Y: (s,1,t)
      s = dir.x / a;
      t = dir.z / a;
    } else {
      f = 3; // -Y: (s,-1,-t)
      s =  dir.x / a;
      t = -dir.z / a;
    }
  } else {
    const Real a = az;
    if (dir.z >= 0) {
      f = 4; // +Z: (s,-t,1)
      s =  dir.x / a;
      t = -dir.y / a;
    } else {
      f = 5; // -Z: (-s,-t,-1)
      s = -dir.x / a;
      t = -dir.y / a;
    }
  }

  *face = f;
  *u = (s + 1) * .5;
  *v = (t + 1) * .5;
}

int DeepShadowMap::Build(const TraceContext &cxt, const ObjectGroup *target,
    const Vector &light_pos, int resolution, Real max_distance)
{
  if (target == NULL || resolution <= 0 || max_distance <= 0) {
    return -1;
  }

  const ObjectSet *surface_set = target->GetSurfaceSet();
  const ObjectSet *volume_set = target->GetVolumeSet();

  // volumes are not part of the deep shadow contract
  if (volume_set->GetObjectCount() != 0) {
    return -1;
  }

  // only triangle mesh surfaces are supported; reject curves and point clouds
  const int nobjects = surface_set->GetObjectCount();
  for (int i = 0; i < nobjects; i++) {
    const ObjectInstance *obj = surface_set->GetObject(i);
    if (!obj->IsSurface()) {
      return -1;
    }
    const int prim_type =
        obj->GetAccelerator()->GetPrimitiveSet()->GetPrimitiveType();
    if (prim_type != PRIMSET_MESH) {
      return -1;
    }
  }

  // shadow context handed to the shaders: secondary traces are disabled so
  // the capture never recurses into the scene
  TraceContext shadow_cxt = cxt;
  shadow_cxt.ray_context = CXT_SHADOW_RAY;
  shadow_cxt.max_diffuse_depth = 0;
  shadow_cxt.max_reflect_depth = 0;
  shadow_cxt.max_refract_depth = 0;

  const int N = resolution;
  std::vector<std::vector<DeepShadowBreakpoint> > staircases(
      CUBE_FACE_COUNT * N * N);

  const Real merge_eps = distance_merge_epsilon(max_distance);
  const Accelerator *top_accelerator = target->GetSurfaceAccelerator();

  for (int face = 0; face < CUBE_FACE_COUNT; face++) {
    for (int y = 0; y < N; y++) {
      for (int x = 0; x < N; x++) {
        Ray ray;
        ray.orig = light_pos;
        ray.dir = texel_direction(face, x, y, N);
        ray.tmin = 0;
        ray.tmax = max_distance;

        // top-level BVH candidate walk: only instances whose bounds the ray
        // crosses are opened, the rest of the scene is never scanned
        std::vector<int> instance_ids;
        top_accelerator->IntersectAllPrimitives(ray, cxt.time, &instance_ids);

        // gather every layer through each candidate mesh BVH/grid walk;
        // no nearest-hit re-query, no epsilon marching
        std::vector<DeepShadowHit> hits;
        for (size_t i = 0; i < instance_ids.size(); i++) {
          const ObjectInstance *obj = surface_set->GetObject(instance_ids[i]);
          std::vector<Intersection> intersections;
          obj->RayIntersectAll(ray, cxt.time, &intersections);

          for (size_t k = 0; k < intersections.size(); k++) {
            const Intersection &isect = intersections[k];

            // world radial distance: independent of any local ray scaling
            const Real distance = Length(isect.P - light_pos);
            if (!(distance > 0 && distance <= max_distance)) {
              continue;
            }

            // shaders see a finite Os under CXT_SHADOW_RAY and cannot trace
            Real Os = 1;
            const Shader *shader = isect.GetShader();
            if (shader != NULL) {
              SurfaceInput in;
              SurfaceOutput out;
              in.shaded_object = isect.object;
              in.P  = isect.P;
              in.N  = isect.N;
              in.Cd = isect.Cd;
              in.uv = isect.uv;
              in.I  = ray.dir;
              in.dPdu = isect.dPdu;
              in.dPdv = isect.dPdv;

              shader->Evaluate(shadow_cxt, in, &out);
              Os = Clamp(static_cast<Real>(out.Os), 0., 1.);
            }

            DeepShadowHit hit;
            hit.distance = distance;
            hit.factor = 1 - Os;
            hit.object = isect.object;
            hit.prim_id = isect.prim_id;
            hits.push_back(hit);
          }
        }

        std::sort(hits.begin(), hits.end(), hit_distance_less);

        // fold equal-distance layers into one right-continuous breakpoint
        std::vector<DeepShadowBreakpoint> &staircase =
            staircases[face * N * N + y * N + x];
        Real transmittance = 1;
        for (size_t k = 0; k < hits.size();) {
          const Real distance = hits[k].distance;
          size_t end = k + 1;
          while (end < hits.size() &&
                 hits[end].distance - distance <= merge_eps) {
            ++end;
          }

          for (size_t j = k; j < end; j++) {
            transmittance *= hits[j].factor;
          }
          staircase.push_back(DeepShadowBreakpoint(distance, transmittance));
          k = end;
        }
      }
    }
  }

  resolution_ = resolution;
  max_distance_ = max_distance;
  time_ = cxt.time;
  target_ = target;
  staircases_.swap(staircases);
  built_ = true;

  return 0;
}

Real DeepShadowMap::staircase_transmittance(
    const std::vector<DeepShadowBreakpoint> &staircase, Real d)
{
  // right-continuous: T(d) includes every breakpoint at distance <= d
  Real transmittance = 1;
  for (size_t i = 0; i < staircase.size(); i++) {
    if (staircase[i].distance <= d) {
      transmittance = staircase[i].transmittance;
    } else {
      break;
    }
  }
  return transmittance;
}

Real DeepShadowMap::Lookup(const Vector &receiver_dir,
    Real distance_minus_bias) const
{
  if (!built_) {
    return 1;
  }

  const int N = resolution_;
  int face = 0;
  Real u = 0;
  Real v = 0;
  direction_to_face_uv(receiver_dir, &face, &u, &v);

  const Real qx = u * N - .5;
  const Real qy = v * N - .5;
  const int base_x = static_cast<int>(Floor(qx));
  const int base_y = static_cast<int>(Floor(qy));
  const Real fx = qx - base_x;
  const Real fy = qy - base_y;

  const Real weights[4] = {
    (1 - fx) * (1 - fy),
    fx * (1 - fy),
    (1 - fx) * fy,
    fx * fy
  };
  const int offset_x[4] = {0, 1, 0, 1};
  const int offset_y[4] = {0, 0, 1, 1};

  Real filtered = 0;
  for (int n = 0; n < 4; n++) {
    int nx = base_x + offset_x[n];
    int ny = base_y + offset_y[n];
    int nface = face;

    if (nx < 0 || nx >= N || ny < 0 || ny >= N) {
      // cross-face neighbour: rebuild its direction on the original face and
      // remap to the face that actually stores it
      const Real uc = (nx + .5) / N;
      const Real vc = (ny + .5) / N;
      const Vector dir = Normalize(face_direction(face, 2 * uc - 1, 2 * vc - 1));

      int new_face = 0;
      Real u2 = 0;
      Real v2 = 0;
      direction_to_face_uv(dir, &new_face, &u2, &v2);

      nx = static_cast<int>(Floor(u2 * N));
      ny = static_cast<int>(Floor(v2 * N));
      nx = Clamp(nx, 0, N - 1);
      ny = Clamp(ny, 0, N - 1);
      nface = new_face;
    }

    const std::vector<DeepShadowBreakpoint> &staircase =
        staircases_[nface * N * N + ny * N + nx];
    filtered += weights[n] *
        staircase_transmittance(staircase, distance_minus_bias);
  }

  return filtered;
}

const std::vector<DeepShadowBreakpoint> &DeepShadowMap::GetStaircase(
    int face, int x, int y) const
{
  return staircases_[face * resolution_ * resolution_ + y * resolution_ + x];
}

} // namespace xxx
