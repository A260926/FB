// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

// Deep shadow map regression tests. Registered with TASK_CHECK into the
// capture, lookup and transport groups; no main() is defined here.

#include "fj_mesh.h"
#include "fj_curve.h"
#include "fj_point_cloud.h"
#include "fj_volume.h"
#include "fj_bvh_accelerator.h"
#include "fj_grid_accelerator.h"
#include "fj_object_instance.h"
#include "fj_object_group.h"
#include "fj_intersection.h"
#include "fj_point_light.h"
#include "fj_deep_shadow.h"
#include "fj_shader.h"
#include "fj_shading.h"
#include "fj_triangle.h"
#include "fj_ray.h"
#include "fj_numeric.h"

#include "../task_support/check.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace fj;

namespace {

const Real BIAS = .0001;

bool near_value(Real a, Real b, Real tol = 1e-6)
{
  return std::fabs(a - b) <= tol;
}

// Mirrors the cube contract: world direction of a texel centre.
Vector cube_texel_dir(int face, int x, int y, int N)
{
  const Real u = (x + .5) / N;
  const Real v = (y + .5) / N;
  const Real s = 2 * u - 1;
  const Real t = 2 * v - 1;
  Vector d;
  switch (face) {
  case 0: d = Vector( 1, -t, -s); break; // +X
  case 1: d = Vector(-1, -t,  s); break; // -X
  case 2: d = Vector( s,  1,  t); break; // +Y
  case 3: d = Vector( s, -1, -t); break; // -Y
  case 4: d = Vector( s, -t,  1); break; // +Z
  default: d = Vector(-s, -t, -1);       // -Z
  }
  return Normalize(d);
}

// Radial distance at which a normalized ray from the origin reaches the
// plane z = z_plane.
Real plane_distance(const Vector &dir, Real z_plane)
{
  return z_plane / dir.z;
}

// Independent re-implementation of the cube contract face selection, used to
// cross-check the production lookup rather than calling into it.
void cube_dir_to_face_uv(const Vector &dir, int *face, Real *u, Real *v)
{
  const Real ax = std::fabs(dir.x);
  const Real ay = std::fabs(dir.y);
  const Real az = std::fabs(dir.z);
  int f = 0;
  Real s = 0, t = 0;

  if (ax >= ay && ax >= az) {
    const Real a = ax;
    if (dir.x >= 0) { f = 0; t = -dir.y / a; s = -dir.z / a; }
    else            { f = 1; t = -dir.y / a; s =  dir.z / a; }
  } else if (ay >= az) {
    const Real a = ay;
    if (dir.y >= 0) { f = 2; s = dir.x / a; t =  dir.z / a; }
    else            { f = 3; s = dir.x / a; t = -dir.z / a; }
  } else {
    const Real a = az;
    if (dir.z >= 0) { f = 4; s =  dir.x / a; t = -dir.y / a; }
    else            { f = 5; s = -dir.x / a; t = -dir.y / a; }
  }
  *face = f;
  *u = (s + 1) * .5;
  *v = (t + 1) * .5;
}

// Right-continuous staircase evaluation from stored breakpoints.
Real staircase_at(const std::vector<DeepShadowBreakpoint> &stairs, Real d)
{
  Real trans = 1;
  for (size_t i = 0; i < stairs.size(); i++) {
    if (stairs[i].distance <= d) trans = stairs[i].transmittance;
    else break;
  }
  return trans;
}

// Reference four-neighbour cross-face filter built only from the stored
// staircases and the documented cube math.
Real reference_lookup(const DeepShadowMap &cache, int N,
    const Vector &dir, Real d)
{
  int face = 0;
  Real u = 0, v = 0;
  cube_dir_to_face_uv(dir, &face, &u, &v);

  const Real qx = u * N - .5;
  const Real qy = v * N - .5;
  const int bx = static_cast<int>(Floor(qx));
  const int by = static_cast<int>(Floor(qy));
  const Real fx = qx - bx;
  const Real fy = qy - by;

  const Real w[4] = {
    (1 - fx) * (1 - fy), fx * (1 - fy),
    (1 - fx) * fy, fx * fy
  };
  const int ox[4] = {0, 1, 0, 1};
  const int oy[4] = {0, 0, 1, 1};

  Real sum = 0;
  for (int n = 0; n < 4; n++) {
    int nx = bx + ox[n];
    int ny = by + oy[n];
    int nf = face;
    if (nx < 0 || nx >= N || ny < 0 || ny >= N) {
      const Vector nd = cube_texel_dir(face, nx, ny, N);
      Real u2 = 0, v2 = 0;
      cube_dir_to_face_uv(nd, &nf, &u2, &v2);
      nx = static_cast<int>(Floor(u2 * N));
      ny = static_cast<int>(Floor(v2 * N));
      nx = nx < 0 ? 0 : (nx >= N ? N - 1 : nx);
      ny = ny < 0 ? 0 : (ny >= N ? N - 1 : ny);
    }
    sum += w[n] * staircase_at(cache.GetStaircase(nf, nx, ny), d);
  }
  return sum;
}

// A shader that reports a fixed opacity, evaluated without any tracing.
class FixedOpacity : public Shader {
public:
  explicit FixedOpacity(float opacity) : opacity_(opacity) {}
private:
  void evaluate(const TraceContext &, const SurfaceInput &,
      SurfaceOutput *out) const
  {
    out->Cs = Color(1, 1, 1);
    out->Os = opacity_;
  }
  float opacity_;
};

// Builds an axis-aligned triangle sheet perpendicular to the z axis at the
// given depth, spanning [-half, half] in x and y.
Mesh *make_sheet(Real z, Real half = 4.)
{
  Mesh *mesh = new Mesh();
  mesh->SetPointCount(3);
  mesh->SetFaceCount(1);
  mesh->AddPointPosition();
  mesh->AddPointNormal();
  mesh->AddFaceIndices();

  mesh->SetPointPosition(0, Vector(-half, -half, z));
  mesh->SetPointPosition(1, Vector( half, -half, z));
  mesh->SetPointPosition(2, Vector( 0,  half, z));
  mesh->SetFaceIndices(0, Index3(0, 1, 2));
  mesh->ComputeNormals();
  mesh->ComputeBounds();
  return mesh;
}

ObjectInstance *make_mesh_instance(Mesh *mesh, Accelerator *acc,
    const Shader *shader)
{
  acc->SetPrimitiveSet(mesh);
  if (acc->Build() != 0) {
    return NULL;
  }

  ObjectInstance *object = new ObjectInstance();
  if (object->SetSurface(acc) != 0) {
    delete object;
    return NULL;
  }
  object->SetShader(shader, 0);
  object->ComputeBounds();
  return object;
}

int build_top_accelerator(ObjectGroup *group)
{
  group->ComputeBounds();
  Accelerator *top = const_cast<Accelerator *>(group->GetSurfaceAccelerator());
  return top->Build();
}

TraceContext make_capture_context(const ObjectGroup *group, Real time = 0)
{
  TraceContext cxt = SlCameraContext(group);
  cxt.ray_context = CXT_SHADOW_RAY;
  cxt.cast_shadow = 1;
  cxt.time = time;
  cxt.trace_target = group;
  return cxt;
}

PointLight *make_light()
{
  PointLight *light = new PointLight();
  light->SetColor(1, 1, 1);
  light->SetIntensity(1);
  if (light->Preprocess() != 0) {
    delete light;
    return NULL;
  }
  return light;
}

// Builds a cache directly around the origin, mirroring PointLight's capture.
DeepShadowMap *build_cache(const TraceContext &cxt, const ObjectGroup *group,
    int resolution, Real max_distance)
{
  DeepShadowMap *cache = new DeepShadowMap();
  if (cache->Build(cxt, group, Vector(0, 0, 0),
      resolution, max_distance) != 0) {
    delete cache;
    return NULL;
  }
  return cache;
}

// Drives a real SlIlluminance call and returns the red channel of the light.
// A negative result signals that the illuminance call did not light the point.
Real illuminance_red(const TraceContext &cxt, const PointLight &light,
    const ObjectGroup *shadow_group, const Vector &receiver,
    const Vector &axis)
{
  ObjectInstance receiver_obj;
  receiver_obj.SetShadowTarget(shadow_group);

  SurfaceInput in;
  in.shaded_object = &receiver_obj;
  in.P = receiver;
  in.N = axis;

  LightSample sample;
  light.GetSamples(&sample, 1);

  LightOutput output;
  const int lit = SlIlluminance(&cxt, &sample, &receiver, &axis,
      PI / 2., &in, &output);
  if (lit != 1) {
    return -1;
  }
  return output.Cl.r;
}

// Looks a receiver direction up through the light's public visibility query.
// A negative result means the cache declined the query.
Real light_visibility(const PointLight &light, const TraceContext &cxt,
    const Vector &receiver)
{
  Real visibility = -1;
  const bool ok = light.LookupShadowVisibility(cxt, receiver, BIAS,
      &visibility);
  return ok ? visibility : -1;
}

} // namespace

// ---------------------------------------------------------------------------
// capture group
// ---------------------------------------------------------------------------

// Two independent translucent sheets produce two staircase breakpoints whose
// transmittance multiplies per layer and can be derived by hand.
static int capture_layered_transmittance()
{
  FixedOpacity shader(.5f);
  BVHAccelerator acc0;
  BVHAccelerator acc1;
  Mesh *mesh0 = make_sheet(1.);
  Mesh *mesh1 = make_sheet(2.);
  ObjectInstance *obj0 = make_mesh_instance(mesh0, &acc0, &shader);
  TASK_CHECK(obj0 != NULL);
  ObjectInstance *obj1 = make_mesh_instance(mesh1, &acc1, &shader);
  TASK_CHECK(obj1 != NULL);

  ObjectGroup group;
  group.AddObject(obj0);
  group.AddObject(obj1);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group);
  DeepShadowMap *cache = build_cache(cxt, &group, 16, 10.);
  TASK_CHECK(cache != NULL);

  // texel (8,8) of face +Z crosses both sheets; its centre is slightly off
  // the +Z axis, so derive the expected radial distances from the contract
  // direction rather than assuming the plane depth
  const Vector texel_dir = cube_texel_dir(4, 8, 8, 16);
  const std::vector<DeepShadowBreakpoint> &stairs =
      cache->GetStaircase(4, 8, 8);
  TASK_CHECK(stairs.size() == 2);
  TASK_CHECK(near_value(stairs[0].distance, plane_distance(texel_dir, 1.)));
  TASK_CHECK(near_value(stairs[0].transmittance, .5));
  TASK_CHECK(near_value(stairs[1].distance, plane_distance(texel_dir, 2.)));
  TASK_CHECK(near_value(stairs[1].transmittance, .25));

  // a texel pointing away from the sheets stays fully open
  const std::vector<DeepShadowBreakpoint> &empty =
      cache->GetStaircase(5, 8, 8);
  TASK_CHECK(empty.empty());

  // the same capture must be reachable through the PointLight entry point
  PointLight *light = make_light();
  TASK_CHECK(light != NULL);
  TASK_CHECK(light->BuildDeepShadowMap(cxt, 16, 10.));
  TASK_CHECK(near_value(light_visibility(*light, cxt, Vector(0, 0, 3.)),
      .25));

  delete light;
  delete cache;
  delete obj1;
  delete obj0;
  delete mesh1;
  delete mesh0;
  return 0;
}

// Two instances sharing one triangle geometry, placed at the same depth, are
// distinct layers yet merge into a single right-continuous breakpoint.
static int capture_identical_distance_instances()
{
  FixedOpacity shader(.5f);
  Mesh *mesh = make_sheet(1.5);

  BVHAccelerator acc_a;
  BVHAccelerator acc_b;
  ObjectInstance *obj_a = make_mesh_instance(mesh, &acc_a, &shader);
  TASK_CHECK(obj_a != NULL);
  ObjectInstance *obj_b = make_mesh_instance(mesh, &acc_b, &shader);
  TASK_CHECK(obj_b != NULL);

  ObjectGroup group;
  group.AddObject(obj_a);
  group.AddObject(obj_b);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group);
  DeepShadowMap *cache = build_cache(cxt, &group, 16, 10.);
  TASK_CHECK(cache != NULL);

  const Vector texel_dir = cube_texel_dir(4, 8, 8, 16);
  const std::vector<DeepShadowBreakpoint> &stairs =
      cache->GetStaircase(4, 8, 8);
  TASK_CHECK(stairs.size() == 1);
  TASK_CHECK(near_value(stairs[0].distance,
      plane_distance(texel_dir, 1.5)));
  // both opacities applied at the one breakpoint: .5 * .5
  TASK_CHECK(near_value(stairs[0].transmittance, .25));

  delete cache;
  delete obj_b;
  delete obj_a;
  delete mesh;
  return 0;
}

// A mesh without a shader is treated as opaque, clamping the staircase to 0.
static int capture_opaque_without_shader()
{
  BVHAccelerator acc;
  Mesh *mesh = make_sheet(1.);
  ObjectInstance *obj = make_mesh_instance(mesh, &acc, NULL);
  TASK_CHECK(obj != NULL);

  ObjectGroup group;
  group.AddObject(obj);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group);
  DeepShadowMap *cache = build_cache(cxt, &group, 16, 10.);
  TASK_CHECK(cache != NULL);

  const std::vector<DeepShadowBreakpoint> &stairs =
      cache->GetStaircase(4, 8, 8);
  TASK_CHECK(stairs.size() == 1);
  TASK_CHECK(near_value(stairs[0].transmittance, 0.));

  delete cache;
  delete obj;
  delete mesh;
  return 0;
}

// Curves, point clouds and volumes are explicitly refused, and so are bad
// arguments. A failed build must leave no usable cache behind.
static int capture_unsupported_and_invalid()
{
  PointLight *light = make_light();
  TASK_CHECK(light != NULL);
  DeepShadowMap cache;

  // curve target
  {
    Curve curve;
    curve.SetVertexCount(4);
    curve.SetCurveCount(1);
    curve.AddVertexPosition();
    curve.AddVertexWidth();
    curve.AddCurveIndices();
    curve.SetVertexPosition(0, Vector(-1, -1, 1));
    curve.SetVertexPosition(1, Vector(-.3, .3, 1));
    curve.SetVertexPosition(2, Vector(.3, -.3, 1));
    curve.SetVertexPosition(3, Vector(1, 1, 1));
    curve.SetVertexWidth(0, .1);
    curve.SetVertexWidth(3, .1);
    curve.SetCurveIndices(0, 0);
    curve.ComputeBounds();

    BVHAccelerator acc;
    acc.SetPrimitiveSet(&curve);
    TASK_CHECK(acc.Build() == 0);
    ObjectInstance object;
    TASK_CHECK(object.SetSurface(&acc) == 0);
    object.ComputeBounds();

    ObjectGroup group;
    group.AddObject(&object);
    TASK_CHECK(build_top_accelerator(&group) == 0);

    TraceContext cxt = make_capture_context(&group);
    TASK_CHECK(cache.Build(cxt, &group, Vector(0, 0, 0), 8, 10.) != 0);
    TASK_CHECK(!light->BuildDeepShadowMap(cxt, 8, 10.));
  }

  // point cloud target
  {
    PointCloud cloud;
    cloud.SetPointCount(1);
    cloud.AddPointPosition();
    cloud.AddPointRadius();
    cloud.SetPointPosition(0, Vector(0, 0, 1));
    cloud.SetPointRadius(0, .1);
    cloud.ComputeBounds();

    BVHAccelerator acc;
    acc.SetPrimitiveSet(&cloud);
    TASK_CHECK(acc.Build() == 0);
    ObjectInstance object;
    TASK_CHECK(object.SetSurface(&acc) == 0);
    object.ComputeBounds();

    ObjectGroup group;
    group.AddObject(&object);
    TASK_CHECK(build_top_accelerator(&group) == 0);

    TraceContext cxt = make_capture_context(&group);
    TASK_CHECK(cache.Build(cxt, &group, Vector(0, 0, 0), 8, 10.) != 0);
    TASK_CHECK(!light->BuildDeepShadowMap(cxt, 8, 10.));
  }

  // volume target
  {
    Volume volume;
    volume.Resize(2, 2, 2);
    volume.SetBounds(Box(Vector(-1, -1, -1), Vector(1, 1, 1)));
    ObjectInstance object;
    TASK_CHECK(object.SetVolume(&volume) == 0);
    object.ComputeBounds();

    ObjectGroup group;
    group.AddObject(&object);
    group.ComputeBounds();

    TraceContext cxt = make_capture_context(&group);
    TASK_CHECK(cache.Build(cxt, &group, Vector(0, 0, 0), 8, 10.) != 0);
    TASK_CHECK(!light->BuildDeepShadowMap(cxt, 8, 10.));
  }

  // invalid resolution and distance, including against a valid mesh group
  {
    BVHAccelerator acc;
    Mesh *mesh = make_sheet(1.);
    ObjectInstance *obj = make_mesh_instance(mesh, &acc, NULL);
    TASK_CHECK(obj != NULL);
    ObjectGroup group;
    group.AddObject(obj);
    TASK_CHECK(build_top_accelerator(&group) == 0);
    TraceContext cxt = make_capture_context(&group);

    TASK_CHECK(cache.Build(cxt, &group, Vector(0, 0, 0), 0, 10.) != 0);
    TASK_CHECK(cache.Build(cxt, &group, Vector(0, 0, 0), -4, 10.) != 0);
    TASK_CHECK(cache.Build(cxt, &group, Vector(0, 0, 0), 8, 0.) != 0);
    TASK_CHECK(cache.Build(cxt, &group, Vector(0, 0, 0), 8, -1.) != 0);
    TASK_CHECK(!light->BuildDeepShadowMap(cxt, 0, 10.));
    TASK_CHECK(!light->BuildDeepShadowMap(cxt, 8, 0.));

    Real unused = 0;
    TASK_CHECK(!light->LookupShadowVisibility(cxt, Vector(0, 0, 1),
        BIAS, &unused));

    delete obj;
    delete mesh;
  }

  delete light;
  return 0;
}

// Over 1024 spatially separated triangles, the BVH candidate walk performs
// far fewer triangle tests than the brute-force rays*triangles budget.
static int capture_acceleration_budget()
{
  const int NTRI = 1100;
  const int RES = 16;
  const int RAYS = 6 * RES * RES;

  Mesh mesh;
  mesh.SetPointCount(3 * NTRI);
  mesh.SetFaceCount(NTRI);
  mesh.AddPointPosition();
  mesh.AddFaceIndices();

  // each triangle gets its own cell spread along a long strip so a ray can
  // only meet a handful of them
  for (int i = 0; i < NTRI; i++) {
    const Real x = i * 1.0;
    mesh.SetPointPosition(3 * i,     Vector(x, -.3, -.3));
    mesh.SetPointPosition(3 * i + 1, Vector(x + .6, -.3, -.3));
    mesh.SetPointPosition(3 * i + 2, Vector(x + .3, .3, -.3));
    mesh.SetFaceIndices(i, Index3(3 * i, 3 * i + 1, 3 * i + 2));
  }
  mesh.ComputeBounds();

  BVHAccelerator acc;
  acc.SetPrimitiveSet(&mesh);
  TASK_CHECK(acc.Build() == 0);
  ObjectInstance object;
  TASK_CHECK(object.SetSurface(&acc) == 0);
  object.ComputeBounds();

  ObjectGroup group;
  group.AddObject(&object);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group);
  PointLight *light = make_light();
  TASK_CHECK(light != NULL);

  TriResetRayTestCount();
  TASK_CHECK(light->BuildDeepShadowMap(cxt, RES, 2000.));
  const long long tests = TriGetRayTestCount();

  const long long brute_force = static_cast<long long>(RAYS) * NTRI;
  std::printf("capture acceleration: rays=%d tris=%d tests=%lld budget=%lld\n",
      RAYS, NTRI, tests, brute_force / 8);
  TASK_CHECK(tests < brute_force / 8);

  delete light;
  return 0;
}

// The grid backend must count a triangle referenced by several cells once
// and still keep the per-layer staircase.
static int capture_grid_dedup_and_layers()
{
  FixedOpacity shader(.25f);
  GridAccelerator acc0;
  GridAccelerator acc1;
  Mesh *mesh0 = make_sheet(1., 2.);
  Mesh *mesh1 = make_sheet(2., 2.);
  ObjectInstance *obj0 = make_mesh_instance(mesh0, &acc0, &shader);
  TASK_CHECK(obj0 != NULL);
  ObjectInstance *obj1 = make_mesh_instance(mesh1, &acc1, &shader);
  TASK_CHECK(obj1 != NULL);

  ObjectGroup group;
  group.AddObject(obj0);
  group.AddObject(obj1);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group);
  DeepShadowMap *cache = build_cache(cxt, &group, 16, 10.);
  TASK_CHECK(cache != NULL);

  const std::vector<DeepShadowBreakpoint> &stairs =
      cache->GetStaircase(4, 8, 8);
  TASK_CHECK(stairs.size() == 2);
  TASK_CHECK(near_value(stairs[0].transmittance, .75));
  TASK_CHECK(near_value(stairs[1].transmittance, .75 * .75));

  delete cache;
  delete obj1;
  delete obj0;
  delete mesh1;
  delete mesh0;
  return 0;
}

// A triangle referenced by several grid cells is reported as exactly one
// candidate and incurs exactly one triangle test for a ray through it.
static int capture_grid_cell_dedup_single_primitive()
{
  const int NSMALL = 200;
  Mesh mesh;
  mesh.SetPointCount(3 + 3 * NSMALL);
  mesh.SetFaceCount(1 + NSMALL);
  mesh.AddPointPosition();
  mesh.AddFaceIndices();

  // one large triangle (id 0) spanning many grid cells around the origin
  mesh.SetPointPosition(0, Vector(-8, -8, 1));
  mesh.SetPointPosition(1, Vector( 8, -8, 1));
  mesh.SetPointPosition(2, Vector( 0,  8, 1));
  mesh.SetFaceIndices(0, Index3(0, 1, 2));

  // many small triangles packed into a compact box beyond the ray tmax so the
  // uniform grid gets small cells and triangle 0 spans several of them
  for (int i = 0; i < NSMALL; i++) {
    const Real cx = -2 + (i % 20) * .2;
    const Real cy = -2 + (i / 20) * .2;
    const int b = 3 + 3 * i;
    mesh.SetPointPosition(b,     Vector(cx,       cy,       5));
    mesh.SetPointPosition(b + 1, Vector(cx + .15, cy,       5));
    mesh.SetPointPosition(b + 2, Vector(cx + .07, cy + .15, 5));
    mesh.SetFaceIndices(i + 1, Index3(b, b + 1, b + 2));
  }
  mesh.ComputeBounds();

  GridAccelerator acc;
  acc.SetPrimitiveSet(&mesh);
  TASK_CHECK(acc.Build() == 0);

  // diagonal ray stays inside triangle 0 while crossing several grid cells
  Ray ray;
  ray.orig = Vector(0, 0, 0);
  ray.dir = Normalize(Vector(2, 0, 1));
  ray.tmin = 0;
  ray.tmax = 3.;

  // candidate walk through the grid: several cells reference primitive 0,
  // but de-duplication leaves exactly one id
  std::vector<int> ids;
  acc.IntersectAllPrimitives(ray, 0, &ids);
  TASK_CHECK(ids.size() == 1);
  TASK_CHECK(ids[0] == 0);

  // and exactly one actual triangle test / intersection for that ray
  ObjectInstance object;
  TASK_CHECK(object.SetSurface(&acc) == 0);
  object.ComputeBounds();

  TriResetRayTestCount();
  std::vector<Intersection> hits;
  object.RayIntersectAll(ray, 0, &hits);
  TASK_CHECK(hits.size() == 1);
  TASK_CHECK(hits[0].prim_id == 0);
  TASK_CHECK(TriGetRayTestCount() == 1);

  return 0;
}

// Instance transform motion and mesh velocity are both sampled at cxt.time,
// so layers land at the time-correct world depths while hit parameters stay
// independent of the local ray direction's length.
static int capture_transform_and_velocity_motion()
{
  const Real sample_time = .5;
  FixedOpacity shader(.5f);

  // instance A: local sheet at z=1 translated to world z=1.5 at sample time
  Mesh *mesh_a = make_sheet(1.);
  BVHAccelerator acc_a;
  ObjectInstance *obj_a = make_mesh_instance(mesh_a, &acc_a, &shader);
  TASK_CHECK(obj_a != NULL);
  obj_a->SetTranslate(0, 0, 0, 0);
  obj_a->SetTranslate(0, 0, 1, 1);
  obj_a->ComputeBounds();

  // instance B: stationary instance, but the vertices carry z velocity
  Mesh *mesh_b = make_sheet(1.);
  mesh_b->AddPointVelocity();
  mesh_b->SetPointVelocity(0, Vector(0, 0, 2));
  mesh_b->SetPointVelocity(1, Vector(0, 0, 2));
  mesh_b->SetPointVelocity(2, Vector(0, 0, 2));
  BVHAccelerator acc_b;
  ObjectInstance *obj_b = make_mesh_instance(mesh_b, &acc_b, &shader);
  TASK_CHECK(obj_b != NULL);

  ObjectGroup group;
  group.AddObject(obj_a);
  group.AddObject(obj_b);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group, sample_time);
  DeepShadowMap *cache = build_cache(cxt, &group, 16, 10.);
  TASK_CHECK(cache != NULL);

  const Vector texel_dir = cube_texel_dir(4, 8, 8, 16);
  const std::vector<DeepShadowBreakpoint> &stairs_motion =
      cache->GetStaircase(4, 8, 8);
  TASK_CHECK(stairs_motion.size() == 2);
  // A: local z=1 plus 0.5 of a unit translate -> world z=1.5
  TASK_CHECK(near_value(stairs_motion[0].distance,
      plane_distance(texel_dir, 1.5)));
  // B: local z=1 plus 0.5 * velocity 2 -> world z=2
  TASK_CHECK(near_value(stairs_motion[1].distance,
      plane_distance(texel_dir, 2.)));
  // each layer multiplies the running transmittance
  TASK_CHECK(near_value(stairs_motion[0].transmittance, .5));
  TASK_CHECK(near_value(stairs_motion[1].transmittance, .25));

  delete cache;
  delete obj_b;
  delete obj_a;
  delete mesh_b;
  delete mesh_a;
  return 0;
}

// ---------------------------------------------------------------------------
// lookup group
// ---------------------------------------------------------------------------

// Right-continuous staircase: the breakpoint applies at its own distance and
// querying at receiver distance minus bias excludes the receiver's own sheet.
static int lookup_staircase_and_bias()
{
  FixedOpacity shader(.5f);
  BVHAccelerator acc0;
  BVHAccelerator acc1;
  Mesh *mesh0 = make_sheet(1.);
  Mesh *mesh1 = make_sheet(2.);
  ObjectInstance *obj0 = make_mesh_instance(mesh0, &acc0, &shader);
  TASK_CHECK(obj0 != NULL);
  ObjectInstance *obj1 = make_mesh_instance(mesh1, &acc1, &shader);
  TASK_CHECK(obj1 != NULL);

  ObjectGroup group;
  group.AddObject(obj0);
  group.AddObject(obj1);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group);
  DeepShadowMap *cache = build_cache(cxt, &group, 16, 10.);
  TASK_CHECK(cache != NULL);

  // before the first sheet: fully visible
  TASK_CHECK(near_value(cache->Lookup(Vector(0, 0, 1), .5 - BIAS), 1.));
  // between the sheets: only the first one attenuates
  TASK_CHECK(near_value(cache->Lookup(Vector(0, 0, 1), 1.5 - BIAS), .5));
  // beyond both: both attenuate
  TASK_CHECK(near_value(cache->Lookup(Vector(0, 0, 1), 3. - BIAS), .25));
  // receiver sitting exactly on the second sheet: bias removes its own layer
  TASK_CHECK(near_value(cache->Lookup(Vector(0, 0, 1), 2. - BIAS), .5));

  delete cache;
  delete obj1;
  delete obj0;
  delete mesh1;
  delete mesh0;
  return 0;
}

// Four-neighbour filtering averages per-texel staircases rather than
// interpolating depths: a direction between a covered and an open region
// yields the bilinear-weighted average of their transmittances.
static int lookup_four_neighbour_filtering()
{
  const int N = 32;
  FixedOpacity shader(.5f);
  BVHAccelerator acc;
  // sheet covering the central +Z texels but leaving the face edges open
  Mesh *mesh = make_sheet(1., .5);

  ObjectInstance *obj = make_mesh_instance(mesh, &acc, &shader);
  TASK_CHECK(obj != NULL);
  ObjectGroup group;
  group.AddObject(obj);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group);
  DeepShadowMap *cache = build_cache(cxt, &group, N, 10.);
  TASK_CHECK(cache != NULL);

  const Real d = 2. - BIAS;

  // a direction landing exactly on a covered texel centre puts all the
  // bilinear weight on that one staircase, returning its .5 transmittance
  const Vector covered_centre = cube_texel_dir(4, N / 2, N / 2, N);
  TASK_CHECK(near_value(cache->Lookup(covered_centre, d), .5, 1e-9));
  TASK_CHECK(near_value(
      reference_lookup(*cache, N, covered_centre, d), .5, 1e-9));

  // the opposite face never meets the sheet and stays fully open
  const Vector back_centre = cube_texel_dir(5, N / 2, N / 2, N);
  TASK_CHECK(near_value(cache->Lookup(back_centre, d), 1., 1e-9));

  // scan the +Z face: production lookup must match the independent reference
  // filter everywhere, and the region must include strict covered/open blends
  bool found_strict_blend = false;
  Real observed_min = 2.;
  Real observed_max = -1.;
  for (int iy = 0; iy < N; iy++) {
    for (int ix = 0; ix < N; ix++) {
      // sample at fractional positions so the four neighbours all contribute
      const Real u = (ix + .25) / N;
      const Real v = (iy + .75) / N;
      const Vector dir = Normalize(Vector(2 * u - 1, -(2 * v - 1), 1));

      const Real got = cache->Lookup(dir, d);
      const Real ref = reference_lookup(*cache, N, dir, d);
      TASK_CHECK(near_value(got, ref, 1e-12));
      TASK_CHECK(got >= 0 && got <= 1 + 1e-9);

      observed_min = Min(observed_min, got);
      observed_max = Max(observed_max, got);
      if (got > .5 + 1e-9 && got < 1. - 1e-9) {
        found_strict_blend = true;
      }
    }
  }
  // some rays are fully attenuated (.5), others fully open (1)
  TASK_CHECK(near_value(observed_min, .5, 1e-9));
  TASK_CHECK(near_value(observed_max, 1., 1e-9));
  // and at least one four-neighbour blend sits strictly between them
  TASK_CHECK(found_strict_blend);

  delete cache;
  delete obj;
  delete mesh;
  return 0;
}

// A direction on a cube face boundary resolves deterministically and never
// indexes outside the stored faces (cross-face neighbour rule).
static int lookup_cross_face_boundary()
{
  FixedOpacity shader(.5f);
  BVHAccelerator acc;
  Mesh *mesh = make_sheet(1., 6.);
  ObjectInstance *obj = make_mesh_instance(mesh, &acc, &shader);
  TASK_CHECK(obj != NULL);
  ObjectGroup group;
  group.AddObject(obj);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group);
  DeepShadowMap *cache = build_cache(cxt, &group, 8, 10.);
  TASK_CHECK(cache != NULL);

  // exact +X/+Z diagonal: tie resolves to +X; the z=1 sheet is still crossed
  const Real v = cache->Lookup(Normalize(Vector(1, 0, 1)), 2. - BIAS);
  TASK_CHECK(near_value(v, .5, 1e-6));

  // directions grazing each of the six face centres stay in range
  const Vector dirs[6] = {
    Vector(1, 0, 0), Vector(-1, 0, 0),
    Vector(0, 1, 0), Vector(0, -1, 0),
    Vector(0, 0, 1), Vector(0, 0, -1)
  };
  for (int i = 0; i < 6; i++) {
    const Real vis = cache->Lookup(dirs[i], 1. - BIAS);
    TASK_CHECK(vis >= 0 && vis <= 1 + 1e-9);
  }

  delete cache;
  delete obj;
  delete mesh;
  return 0;
}

// Mismatched group, time or coverage distance forces a fallback (false), and
// clearing the cache does the same.
static int lookup_cache_invalidation()
{
  FixedOpacity shader(.5f);
  BVHAccelerator acc;
  Mesh *mesh = make_sheet(1.);
  ObjectInstance *obj = make_mesh_instance(mesh, &acc, &shader);
  TASK_CHECK(obj != NULL);
  ObjectGroup group;
  group.AddObject(obj);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group);
  PointLight *light = make_light();
  TASK_CHECK(light != NULL);
  TASK_CHECK(light->BuildDeepShadowMap(cxt, 16, 5.));

  Real vis = -1;
  TASK_CHECK(light->LookupShadowVisibility(cxt, Vector(0, 0, 2.), BIAS, &vis));

  // receiver beyond the captured distance: fallback
  TASK_CHECK(!light->LookupShadowVisibility(cxt, Vector(0, 0, 6.),
      BIAS, &vis));

  // different target group: fallback
  ObjectGroup other;
  TraceContext other_cxt = make_capture_context(&other);
  TASK_CHECK(!light->LookupShadowVisibility(other_cxt, Vector(0, 0, 2.),
      BIAS, &vis));

  // different time: fallback
  TraceContext time_cxt = cxt;
  time_cxt.time = .25;
  TASK_CHECK(!light->LookupShadowVisibility(time_cxt, Vector(0, 0, 2.),
      BIAS, &vis));

  // explicit clear: fallback
  light->ClearDeepShadowMap();
  TASK_CHECK(!light->LookupShadowVisibility(cxt, Vector(0, 0, 2.),
      BIAS, &vis));

  delete light;
  delete obj;
  delete mesh;
  return 0;
}

// Any transformation entry point on the Light base class drops the cache.
static int lookup_transform_invalidates()
{
  FixedOpacity shader(.5f);
  BVHAccelerator acc;
  Mesh *mesh = make_sheet(1.);
  ObjectInstance *obj = make_mesh_instance(mesh, &acc, &shader);
  TASK_CHECK(obj != NULL);
  ObjectGroup group;
  group.AddObject(obj);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext cxt = make_capture_context(&group);
  PointLight *light = make_light();
  TASK_CHECK(light != NULL);
  const Vector receiver(0, 0, 2.);
  Real vis = -1;

  Light *base = light;

  TASK_CHECK(light->BuildDeepShadowMap(cxt, 16, 5.));
  TASK_CHECK(light->LookupShadowVisibility(cxt, receiver, BIAS, &vis));
  base->SetTranslate(1, 0, 0, 0);
  TASK_CHECK(!light->LookupShadowVisibility(cxt, receiver, BIAS, &vis));

  TASK_CHECK(light->BuildDeepShadowMap(cxt, 16, 5.));
  base->SetRotate(0, 10, 0, 0);
  TASK_CHECK(!light->LookupShadowVisibility(cxt, receiver, BIAS, &vis));

  TASK_CHECK(light->BuildDeepShadowMap(cxt, 16, 5.));
  base->SetScale(1, 1, 2, 0);
  TASK_CHECK(!light->LookupShadowVisibility(cxt, receiver, BIAS, &vis));

  TASK_CHECK(light->BuildDeepShadowMap(cxt, 16, 5.));
  base->SetTransformOrder(ORDER_TRS);
  TASK_CHECK(!light->LookupShadowVisibility(cxt, receiver, BIAS, &vis));

  TASK_CHECK(light->BuildDeepShadowMap(cxt, 16, 5.));
  base->SetRotateOrder(ORDER_ZYX);
  TASK_CHECK(!light->LookupShadowVisibility(cxt, receiver, BIAS, &vis));

  delete light;
  delete obj;
  delete mesh;
  return 0;
}

// ---------------------------------------------------------------------------
// transport group
// ---------------------------------------------------------------------------

// With a single sheet the cache path and the regular shadow ray path have
// identical semantics: real SlIlluminance returns the same value, and the
// cached query provably performs no triangle tests.
static int transport_illuminance_matches_cache_and_fallback()
{
  FixedOpacity shader(.5f);
  BVHAccelerator acc;
  Mesh *mesh = make_sheet(1.);
  ObjectInstance *obj = make_mesh_instance(mesh, &acc, &shader);
  TASK_CHECK(obj != NULL);

  ObjectGroup group;
  group.AddObject(obj);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext camera = SlCameraContext(&group);
  const Vector receiver(0, 0, 3.);
  const Vector axis(0, 0, -1);

  PointLight *light = make_light();
  TASK_CHECK(light != NULL);

  // no cache: the existing shadow ray path attenuates by the one sheet
  const Real fallback_light = illuminance_red(camera, *light, &group,
      receiver, axis);
  TASK_CHECK(near_value(fallback_light, .5, 1e-5));

  // with the cache: identical result, but the lookup path does no tracing
  TraceContext capture = make_capture_context(&group);
  TASK_CHECK(light->BuildDeepShadowMap(capture, 16, 10.));

  TriResetRayTestCount();
  const Real cached_light = illuminance_red(camera, *light, &group,
      receiver, axis);
  TASK_CHECK(near_value(cached_light, .5, 1e-5));
  TASK_CHECK(TriGetRayTestCount() == 0);

  // after clearing, the shadow ray path returns and traces again
  light->ClearDeepShadowMap();
  const Real cleared_light = illuminance_red(camera, *light, &group,
      receiver, axis);
  TASK_CHECK(near_value(cleared_light, .5, 1e-5));

  delete light;
  delete obj;
  delete mesh;
  return 0;
}

// Two translucent sheets show what a nearest-depth shadow cannot: the cached
// SlIlluminance accumulates both layers (.5*.5=.25) with no tracing, whereas
// the fallback shadow ray stops at the nearest sheet (.5). The deep result is
// the point of the new path and is verified through the real lighting call.
static int transport_deep_multilayer_accumulation()
{
  FixedOpacity shader(.5f);
  BVHAccelerator acc0;
  BVHAccelerator acc1;
  Mesh *mesh0 = make_sheet(1.);
  Mesh *mesh1 = make_sheet(2.);
  ObjectInstance *obj0 = make_mesh_instance(mesh0, &acc0, &shader);
  TASK_CHECK(obj0 != NULL);
  ObjectInstance *obj1 = make_mesh_instance(mesh1, &acc1, &shader);
  TASK_CHECK(obj1 != NULL);

  ObjectGroup group;
  group.AddObject(obj0);
  group.AddObject(obj1);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext camera = SlCameraContext(&group);
  const Vector receiver(0, 0, 3.);
  const Vector axis(0, 0, -1);

  PointLight *light = make_light();
  TASK_CHECK(light != NULL);

  const Real fallback_light = illuminance_red(camera, *light, &group,
      receiver, axis);
  TASK_CHECK(near_value(fallback_light, .5, 1e-5));

  TraceContext capture = make_capture_context(&group);
  TASK_CHECK(light->BuildDeepShadowMap(capture, 16, 10.));

  TriResetRayTestCount();
  const Real deep_light = illuminance_red(camera, *light, &group,
      receiver, axis);
  TASK_CHECK(near_value(deep_light, .25, 1e-5));
  TASK_CHECK(TriGetRayTestCount() == 0);

  delete light;
  delete obj1;
  delete obj0;
  delete mesh1;
  delete mesh0;
  return 0;
}

// With cast_shadow off, the cache is never consulted and no shadow is applied.
static int transport_cast_shadow_disabled()
{
  FixedOpacity shader(.5f);
  BVHAccelerator acc;
  Mesh *mesh = make_sheet(1.);
  ObjectInstance *obj = make_mesh_instance(mesh, &acc, &shader);
  TASK_CHECK(obj != NULL);
  ObjectGroup group;
  group.AddObject(obj);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext capture = make_capture_context(&group);
  PointLight *light = make_light();
  TASK_CHECK(light != NULL);
  TASK_CHECK(light->BuildDeepShadowMap(capture, 16, 10.));

  TraceContext camera = SlCameraContext(&group);
  camera.cast_shadow = 0;

  TriResetRayTestCount();
  const Real lit = illuminance_red(camera, *light, &group,
      Vector(0, 0, 3.), Vector(0, 0, -1));
  TASK_CHECK(near_value(lit, 1., 1e-6));
  TASK_CHECK(TriGetRayTestCount() == 0);

  delete light;
  delete obj;
  delete mesh;
  return 0;
}

// A receiver outside the captured range falls back to ordinary shadow rays,
// so the cache never clips lighting it was not built to cover.
static int transport_out_of_range_fallback()
{
  FixedOpacity shader(.5f);
  BVHAccelerator acc;
  Mesh *mesh = make_sheet(1.);
  ObjectInstance *obj = make_mesh_instance(mesh, &acc, &shader);
  TASK_CHECK(obj != NULL);
  ObjectGroup group;
  group.AddObject(obj);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext capture = make_capture_context(&group);
  PointLight *light = make_light();
  TASK_CHECK(light != NULL);
  TASK_CHECK(light->BuildDeepShadowMap(capture, 16, 2.));

  TraceContext camera = SlCameraContext(&group);
  TriResetRayTestCount();
  const Real lit = illuminance_red(camera, *light, &group,
      Vector(0, 0, 5.), Vector(0, 0, -1));
  TASK_CHECK(near_value(lit, .5, 1e-5));
  // fallback path had to trace the sheet
  TASK_CHECK(TriGetRayTestCount() > 0);

  delete light;
  delete obj;
  delete mesh;
  return 0;
}

// A built cache supports concurrent read-only lookups from many threads with
// no geometry access and identical results for identical queries.
static int transport_concurrent_readonly_queries()
{
  FixedOpacity shader(.5f);
  BVHAccelerator acc0;
  BVHAccelerator acc1;
  Mesh *mesh0 = make_sheet(1.);
  Mesh *mesh1 = make_sheet(2.);
  ObjectInstance *obj0 = make_mesh_instance(mesh0, &acc0, &shader);
  TASK_CHECK(obj0 != NULL);
  ObjectInstance *obj1 = make_mesh_instance(mesh1, &acc1, &shader);
  TASK_CHECK(obj1 != NULL);

  ObjectGroup group;
  group.AddObject(obj0);
  group.AddObject(obj1);
  TASK_CHECK(build_top_accelerator(&group) == 0);

  TraceContext capture = make_capture_context(&group);
  PointLight *light = make_light();
  TASK_CHECK(light != NULL);
  TASK_CHECK(light->BuildDeepShadowMap(capture, 16, 10.));
  TraceContext query = make_capture_context(&group);

  const int nthreads = 8;
  const int per_thread = 2000;
  std::atomic<int> mismatches(0);
  std::vector<std::thread> workers;

  for (int t = 0; t < nthreads; t++) {
    workers.push_back(std::thread([&, t]() {
      for (int i = 0; i < per_thread; i++) {
        const Real z = 1. + static_cast<Real>((i + t) % 20) * .1;
        const Vector receiver(0, 0, z);
        Real vis = -1;
        if (!light->LookupShadowVisibility(query, receiver, BIAS, &vis)) {
          ++mismatches;
          continue;
        }
        // analytic answer: .25 beyond both sheets, .5 between them, 1 before
        Real expected = 1;
        if (z > 2.) expected = .25;
        else if (z > 1.) expected = .5;
        if (!near_value(vis, expected, 1e-9)) {
          ++mismatches;
        }
      }
    }));
  }
  for (int t = 0; t < nthreads; t++) {
    workers[t].join();
  }
  TASK_CHECK(mismatches.load() == 0);

  // concurrent reads never touched geometry
  TriResetRayTestCount();
  Real vis = -1;
  TASK_CHECK(light->LookupShadowVisibility(query, Vector(0, 0, 3.),
      BIAS, &vis));
  TASK_CHECK(TriGetRayTestCount() == 0);

  delete light;
  delete obj1;
  delete obj0;
  delete mesh1;
  delete mesh0;
  return 0;
}

const struct task_case task_cases[] = {
  // capture
  {"capture", "layered_transmittance", capture_layered_transmittance},
  {"capture", "identical_distance_instances", capture_identical_distance_instances},
  {"capture", "opaque_without_shader", capture_opaque_without_shader},
  {"capture", "unsupported_and_invalid", capture_unsupported_and_invalid},
  {"capture", "acceleration_budget", capture_acceleration_budget},
  {"capture", "grid_dedup_and_layers", capture_grid_dedup_and_layers},
  {"capture", "grid_cell_dedup_single_primitive",
      capture_grid_cell_dedup_single_primitive},
  {"capture", "transform_and_velocity_motion",
      capture_transform_and_velocity_motion},
  // lookup
  {"lookup", "staircase_and_bias", lookup_staircase_and_bias},
  {"lookup", "four_neighbour_filtering", lookup_four_neighbour_filtering},
  {"lookup", "cross_face_boundary", lookup_cross_face_boundary},
  {"lookup", "cache_invalidation", lookup_cache_invalidation},
  {"lookup", "transform_invalidates", lookup_transform_invalidates},
  // transport
  {"transport", "illuminance_matches_cache_and_fallback",
      transport_illuminance_matches_cache_and_fallback},
  {"transport", "deep_multilayer_accumulation",
      transport_deep_multilayer_accumulation},
  {"transport", "cast_shadow_disabled", transport_cast_shadow_disabled},
  {"transport", "out_of_range_fallback", transport_out_of_range_fallback},
  {"transport", "concurrent_readonly_queries",
      transport_concurrent_readonly_queries},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
