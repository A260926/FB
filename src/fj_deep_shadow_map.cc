// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#include "fj_deep_shadow_map.h"
#include "fj_object_instance.h"
#include "fj_primitive_set.h"
#include "fj_intersection.h"
#include "fj_object_group.h"
#include "fj_object_set.h"
#include "fj_accelerator.h"
#include "fj_volume_accelerator.h"
#include "fj_shading.h"
#include "fj_numeric.h"
#include "fj_shader.h"
#include "fj_mesh.h"
#include "fj_ray.h"

#include <algorithm>
#include <cmath>

namespace fj {

// Cube layout shared by build and lookup. Faces, in order:
// +X, -X, +Y, -Y, +Z, -Z. With s=2u-1 and t=2v-1 the inverse directions
// are (1,-t,-s), (-1,-t,s), (s,1,t), (s,-1,-t), (s,-t,1), (-s,-t,-1).
enum CubeFace {
  FACE_POS_X = 0,
  FACE_NEG_X,
  FACE_POS_Y,
  FACE_NEG_Y,
  FACE_POS_Z,
  FACE_NEG_Z
};

static Vector cube_face_direction(int face, Real u, Real v)
{
  const Real s = 2 * u - 1;
  const Real t = 2 * v - 1;

  switch (face) {
  case FACE_POS_X: return Vector( 1, -t, -s);
  case FACE_NEG_X: return Vector(-1, -t,  s);
  case FACE_POS_Y: return Vector( s,  1,  t);
  case FACE_NEG_Y: return Vector( s, -1, -t);
  case FACE_POS_Z: return Vector( s, -t,  1);
  case FACE_NEG_Z: return Vector(-s, -t, -1);
  default:         return Vector( 0,  0,  1);
  }
}

static void cube_direction_to_face_uv(const Vector &dir,
    int *face, Real *u, Real *v)
{
  const Real ax = Abs(dir.x);
  const Real ay = Abs(dir.y);
  const Real az = Abs(dir.z);

  // largest absolute component; ties break X before Y before Z
  int selected = FACE_POS_Z;
  if (ax >= ay && ax >= az) {
    selected = dir.x >= 0 ? FACE_POS_X : FACE_NEG_X;
  } else if (ay >= az) {
    selected = dir.y >= 0 ? FACE_POS_Y : FACE_NEG_Y;
  } else {
    selected = dir.z >= 0 ? FACE_POS_Z : FACE_NEG_Z;
  }

  Real s = 0;
  Real t = 0;
  switch (selected) {
  case FACE_POS_X: s = -dir.z / dir.x; t = -dir.y / dir.x; break;
  case FACE_NEG_X: s = -dir.z / dir.x; t =  dir.y / dir.x; break;
  case FACE_POS_Y: s =  dir.x / dir.y; t =  dir.z / dir.y; break;
  case FACE_NEG_Y: s = -dir.x / dir.y; t =  dir.z / dir.y; break;
  case FACE_POS_Z: s =  dir.x / dir.z; t = -dir.y / dir.z; break;
  case FACE_NEG_Z: s =  dir.x / dir.z; t =  dir.y / dir.z; break;
  default: break;
  }

  *face = selected;
  *u = (s + 1) / 2;
  *v = (t + 1) / 2;
}

// one occluding layer found while capturing a texel
struct DeepShadowLayer {
  DeepShadowLayer(Real dist, Real factor) : distance(dist), transmit(factor) {}
  Real distance;
  Real transmit;
};

static bool layer_less(const DeepShadowLayer &a, const DeepShadowLayer &b)
{
  return a.distance < b.distance;
}

DeepShadowMap::DeepShadowMap() :
    texels_(),
    bp_distance_(),
    bp_transmission_(),
    light_position_(),
    trace_target_(NULL),
    time_(0),
    max_distance_(0),
    resolution_(0),
    has_built_(false)
{
}

DeepShadowMap::~DeepShadowMap()
{
}

void DeepShadowMap::Clear()
{
  std::vector<Texel>().swap(texels_);
  std::vector<Real>().swap(bp_distance_);
  std::vector<Real>().swap(bp_transmission_);

  light_position_ = Vector(0, 0, 0);
  trace_target_ = NULL;
  time_ = 0;
  max_distance_ = 0;
  resolution_ = 0;
  has_built_ = false;
}

bool DeepShadowMap::HasBuilt() const
{
  return has_built_;
}

int DeepShadowMap::GetResolution() const
{
  return resolution_;
}

Real DeepShadowMap::GetMaxDistance() const
{
  return max_distance_;
}

// Every object in the target group must be a triangle mesh sheet;
// curves, point clouds and volumes are rejected explicitly.
static bool validate_trace_target(const ObjectGroup *group)
{
  const Accelerator *top = group->GetSurfaceAccelerator();
  if (top == NULL) {
    return false;
  }

  const PrimitiveSet *primset = top->GetPrimitiveSet();
  const ObjectSet *objset = dynamic_cast<const ObjectSet *>(primset);
  if (objset != NULL) {
    const Index nobjects = objset->GetObjectCount();
    for (Index i = 0; i < nobjects; i++) {
      const ObjectInstance *obj = objset->GetObject(i);
      if (!obj->IsSurface()) {
        return false;
      }
      const Accelerator *acc = obj->GetAccelerator();
      if (acc == NULL) {
        return false;
      }
      const Mesh *mesh = dynamic_cast<const Mesh *>(acc->GetPrimitiveSet());
      if (mesh == NULL) {
        return false;
      }
    }
  }

  const VolumeAccelerator *volume_acc = group->GetVolumeAccelerator();
  if (volume_acc != NULL && volume_acc->num_volumes_ > 0) {
    return false;
  }

  return true;
}

bool DeepShadowMap::Build(const TraceContext &cxt, const Vector &light_position,
    int resolution, Real max_distance)
{
  Clear();

  if (resolution <= 0 || max_distance <= 0) {
    return false;
  }
  const ObjectGroup *group = cxt.trace_target;
  if (group == NULL) {
    return false;
  }
  if (!validate_trace_target(group)) {
    return false;
  }

  const Accelerator *top = group->GetSurfaceAccelerator();

  // shaders are evaluated as shadow rays and must not recurse
  TraceContext shadow_cxt = cxt;
  shadow_cxt.ray_context = CXT_SHADOW_RAY;
  shadow_cxt.max_diffuse_depth = 0;
  shadow_cxt.max_reflect_depth = 0;
  shadow_cxt.max_refract_depth = 0;

  const int N = resolution;
  texels_.resize(6 * N * N);

  std::vector<Intersection> hits;
  std::vector<DeepShadowLayer> layers;

  for (int face = 0; face < 6; face++) {
    for (int y = 0; y < N; y++) {
      for (int x = 0; x < N; x++) {
        const Real u = (x + 0.5) / N;
        const Real v = (y + 0.5) / N;

        Ray ray;
        ray.orig = light_position;
        ray.dir = Normalize(cube_face_direction(face, u, v));
        ray.tmin = 0;
        ray.tmax = max_distance;

        // collect every intersection along (0, max_distance] through the
        // existing top-level and per-object accelerator traversal
        hits.clear();
        top->IntersectAll(ray, cxt.time, &hits);

        layers.clear();
        for (size_t i = 0; i < hits.size(); i++) {
          const Intersection &isect = hits[i];
          // ray.dir is normalized, so t_hit is the radial distance
          if (!(isect.t_hit > 0 && isect.t_hit <= max_distance)) {
            continue;
          }

          Real Os = 1;
          const Shader *shader = NULL;
          if (isect.object != NULL) {
            shader = isect.GetShader();
          }
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
            if (!std::isfinite(static_cast<Real>(out.Os))) {
              Clear();
              return false;
            }
            Os = out.Os;
          }

          // no shader means opaque
          layers.push_back(DeepShadowLayer(isect.t_hit, 1 - Clamp(Os, 0, 1)));
        }

        // merge layers at identical distance into one breakpoint
        std::sort(layers.begin(), layers.end(), layer_less);

        const size_t texel_index = (face * N + y) * N + x;
        const uint32_t offset = static_cast<uint32_t>(bp_distance_.size());
        uint32_t count = 0;

        Real transmittance = 1;
        size_t head = 0;
        while (head < layers.size()) {
          const Real distance = layers[head].distance;
          Real factor = 1;
          while (head < layers.size() && layers[head].distance == distance) {
            factor *= layers[head].transmit;
            head++;
          }
          transmittance *= factor;
          bp_distance_.push_back(distance);
          bp_transmission_.push_back(transmittance);
          count++;
        }

        texels_[texel_index] = Texel(offset, count);
      }
    }
  }

  light_position_ = light_position;
  trace_target_ = group;
  time_ = cxt.time;
  max_distance_ = max_distance;
  resolution_ = resolution;
  has_built_ = true;

  return true;
}

Real DeepShadowMap::eval_texel(int face, int x, int y, Real distance) const
{
  const int N = resolution_;
  const Texel &texel = texels_[(face * N + y) * N + x];

  if (texel.count == 0) {
    return 1;
  }

  // last breakpoint at or before distance; the staircase is
  // right-continuous, so breakpoints at distance <= d are included
  std::vector<Real>::const_iterator begin =
      bp_distance_.begin() + texel.offset;
  std::vector<Real>::const_iterator end = begin + texel.count;
  std::vector<Real>::const_iterator it =
      std::upper_bound(begin, end, distance);

  if (it == begin) {
    return 1;
  }

  const size_t index = (it - begin) - 1;
  return bp_transmission_[texel.offset + index];
}

bool DeepShadowMap::Lookup(const TraceContext &cxt, const Vector &receiver,
    Real bias, Real *visibility) const
{
  if (!has_built_) {
    return false;
  }
  if (cxt.trace_target != trace_target_) {
    return false;
  }
  if (cxt.time != time_) {
    return false;
  }

  const Vector to_receiver = receiver - light_position_;
  const Real distance = Length(to_receiver);
  if (!(distance > 0)) {
    return false;
  }
  if (distance > max_distance_) {
    return false;
  }

  int face = 0;
  Real u = 0;
  Real v = 0;
  cube_direction_to_face_uv(to_receiver, &face, &u, &v);

  const int N = resolution_;
  const Real qx = u * N - 0.5;
  const Real qy = v * N - 0.5;
  const int fx = static_cast<int>(Floor(qx));
  const int fy = static_cast<int>(Floor(qy));
  const Real wx = qx - fx;
  const Real wy = qy - fy;

  // each neighbour's staircase is evaluated at the receiver's radial
  // distance minus bias, then the four values are averaged
  const Real query_distance = distance - bias;

  Real sum = 0;
  for (int j = 0; j < 2; j++) {
    for (int i = 0; i < 2; i++) {
      const int nx = fx + i;
      const int ny = fy + j;
      const Real weight = (i ? wx : 1 - wx) * (j ? wy : 1 - wy);

      int texel_face = face;
      int texel_x = nx;
      int texel_y = ny;

      if (nx < 0 || nx >= N || ny < 0 || ny >= N) {
        // out-of-face neighbour: turn its texel centre back into a
        // direction on the original face and select the new face
        const Real uc = (nx + 0.5) / N;
        const Real vc = (ny + 0.5) / N;
        const Vector neighbour_dir = cube_face_direction(face, uc, vc);

        Real nu = 0;
        Real nv = 0;
        cube_direction_to_face_uv(neighbour_dir, &texel_face, &nu, &nv);

        texel_x = Clamp(static_cast<int>(Floor(nu * N)), 0, N - 1);
        texel_y = Clamp(static_cast<int>(Floor(nv * N)), 0, N - 1);
      }

      sum += weight * eval_texel(texel_face, texel_x, texel_y, query_distance);
    }
  }

  *visibility = sum;
  return true;
}

} // namespace xxx
