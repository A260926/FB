// Deep shadow map regression tests for PointLight.
// Registered with task_support/check.h; no main() here.

#include "task_support/check.h"

#include "fj_mesh.h"
#include "fj_bvh_accelerator.h"
#include "fj_grid_accelerator.h"
#include "fj_object_instance.h"
#include "fj_object_group.h"
#include "fj_intersection.h"
#include "fj_point_light.h"
#include "fj_transform.h"
#include "fj_shader.h"
#include "fj_shading.h"
#include "fj_triangle.h"
#include "fj_curve.h"
#include "fj_point_cloud.h"
#include "fj_volume.h"

#include <cmath>
#include <limits>
#include <thread>
#include <vector>

using namespace fj;

namespace {

class ConstOpacity : public Shader {
public:
  explicit ConstOpacity(float os) : os_(os) {}
  void SetOpacity(float os) { os_ = os; }

private:
  virtual void evaluate(const TraceContext &, const SurfaceInput &,
      SurfaceOutput *out) const
  {
    out->Cs = Color(1, 1, 1);
    out->Os = os_;
  }

  float os_;
};

// one mesh sheet plus the objects needed to put it in a group
struct Sheet {
  Sheet() : shader(1) {}

  Mesh mesh;
  BVHAccelerator bvh;
  GridAccelerator grid;
  ObjectInstance object;
  ConstOpacity shader;
};

void make_single_triangle(Mesh *mesh, const Vector &a, const Vector &b,
    const Vector &c)
{
  mesh->SetPointCount(3);
  mesh->SetFaceCount(1);
  mesh->AddPointPosition();
  mesh->AddFaceIndices();
  mesh->SetPointPosition(0, a);
  mesh->SetPointPosition(1, b);
  mesh->SetPointPosition(2, c);
  mesh->SetFaceIndices(0, Index3(0, 1, 2));
  mesh->ComputeNormals();
  mesh->ComputeBounds();
}

// a single triangle in plane z that covers the central view region;
// sheets are single triangles so capture rays never land on a shared
// triangle edge
void make_tri_sheet(Mesh *mesh, Real z)
{
  make_single_triangle(mesh,
      Vector(-20, -20, z), Vector(20, -20, z), Vector(0, 20, z));
}

int setup_sheet(Sheet *sheet, bool use_grid, float os, ObjectGroup *group)
{
  sheet->shader.SetOpacity(os);
  Accelerator *acc = use_grid
      ? static_cast<Accelerator *>(&sheet->grid)
      : static_cast<Accelerator *>(&sheet->bvh);
  acc->SetPrimitiveSet(&sheet->mesh);
  if (acc->Build() != 0)
    return 1;
  sheet->object.SetSurface(acc);
  sheet->object.SetShader(&sheet->shader, 0);
  sheet->object.ComputeBounds();
  group->AddObject(&sheet->object);
  return 0;
}

int build_top(ObjectGroup *group)
{
  group->ComputeBounds();
  Accelerator *top = const_cast<Accelerator *>(group->GetSurfaceAccelerator());
  return top->Build() != 0;
}

bool close_to(Real a, Real b)
{
  return std::fabs(a - b) < 1e-9;
}

// ---------------------------------------------------------------- capture

int layered_transmittance_with_backend(bool use_grid)
{
  Sheet sheets[3];
  ObjectGroup group;
  const float os[3] = {0.5f, 0.25f, 0.75f};

  for (int i = 0; i < 3; i++) {
    make_tri_sheet(&sheets[i].mesh, -(i + 1));
    if (setup_sheet(&sheets[i], use_grid, os[i], &group))
      return 1;
  }
  if (build_top(&group))
    return 1;

  PointLight light;
  TraceContext cxt = SlCameraContext(&group);
  TASK_CHECK(light.BuildDeepShadowMap(cxt, 4, 10));

  // the four bilinear neighbours of the -Z axis all cast rays with
  // direction (+-0.25, +-0.25, -1)/sqrt(1.125), so the sheets at z=-1,-2,-3
  // leave breakpoints at 1.06, 2.12, 3.18 and transmittances
  // 0.5, 0.5*0.75, 0.5*0.75*0.25
  const struct {
    Real dist;
    Real expect;
  } probes[] = {
    {0.5, 1.0},
    {1.0, 1.0},
    {1.5, 0.5},
    {2.5, 0.375},
    {3.5, 0.09375},
  };
  for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
    Real vis = -1;
    TASK_CHECK(light.LookupShadowVisibility(cxt,
        Vector(0, 0, -probes[p].dist), 0, &vis));
    TASK_CHECK(close_to(vis, probes[p].expect));
  }
  return 0;
}

int capture_layered_transmittance()
{
  if (layered_transmittance_with_backend(false))
    return 1;
  if (layered_transmittance_with_backend(true))
    return 1;
  return 0;
}

int capture_same_distance_instances()
{
  Sheet sheets[2];
  ObjectGroup group;

  // two distinct meshes (distinct source triangles, distinct instances)
  // covering the same plane: one merged breakpoint with both factors
  make_tri_sheet(&sheets[0].mesh, -1);
  make_single_triangle(&sheets[1].mesh,
      Vector(-20, 20, -1), Vector(20, 20, -1), Vector(0, -20, -1));
  for (int i = 0; i < 2; i++) {
    if (setup_sheet(&sheets[i], false, 0.5f, &group))
      return 1;
  }
  if (build_top(&group))
    return 1;

  PointLight light;
  TraceContext cxt = SlCameraContext(&group);
  TASK_CHECK(light.BuildDeepShadowMap(cxt, 4, 10));

  Real vis = -1;
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -1.0), 0, &vis));
  TASK_CHECK(close_to(vis, 1.0));
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -1.5), 0, &vis));
  TASK_CHECK(close_to(vis, 0.25));
  return 0;
}

int capture_grid_duplicate_counts_once()
{
  Sheet sheet;
  ObjectGroup group;

  // a huge tilted triangle: its bounds cover several grid cells and the
  // query ray crosses two of them, so the same source triangle is a
  // candidate twice; it must still be a single layer
  make_single_triangle(&sheet.mesh,
      Vector(-30, -30, -31), Vector(30, -30, -1), Vector(0, 30, -16));
  if (setup_sheet(&sheet, true, 0.5f, &group))
    return 1;
  if (build_top(&group))
    return 1;

  PointLight light;
  TraceContext cxt = SlCameraContext(&group);
  TASK_CHECK(light.BuildDeepShadowMap(cxt, 4, 100));

  // the triangle plane is -x + 2z + 32 = 0; the neighbour texel rays hit
  // it at radial distance ~14.5-14.8
  const Vector dir = Normalize(Vector(0.3, 0, -1));
  Real vis = -1;
  TASK_CHECK(light.LookupShadowVisibility(cxt, dir * 10, 0, &vis));
  TASK_CHECK(close_to(vis, 1.0));
  TASK_CHECK(light.LookupShadowVisibility(cxt, dir * 20, 0, &vis));
  TASK_CHECK(close_to(vis, 0.5));
  return 0;
}

int capture_rejects_unsupported()
{
  // non-positive resolution or max distance, and null target
  {
    Sheet sheet;
    ObjectGroup group;
    make_tri_sheet(&sheet.mesh, -1);
    if (setup_sheet(&sheet, false, 0.5f, &group))
      return 1;
    if (build_top(&group))
      return 1;

    PointLight light;
    TraceContext cxt = SlCameraContext(&group);
    TASK_CHECK(!light.BuildDeepShadowMap(cxt, 0, 10));
    TASK_CHECK(!light.BuildDeepShadowMap(cxt, -4, 10));
    TASK_CHECK(!light.BuildDeepShadowMap(cxt, 4, 0));
    TASK_CHECK(!light.BuildDeepShadowMap(cxt, 4, -10));
    TASK_CHECK(!light.HasDeepShadowMap());

    TraceContext null_cxt = SlCameraContext(NULL);
    TASK_CHECK(!light.BuildDeepShadowMap(null_cxt, 4, 10));

    // a valid build still works afterwards
    TASK_CHECK(light.BuildDeepShadowMap(cxt, 4, 10));
    TASK_CHECK(light.HasDeepShadowMap());
  }

  // curve target is rejected
  {
    Curve curve;
    BVHAccelerator acc;
    acc.SetPrimitiveSet(&curve);
    ObjectInstance obj;
    obj.SetSurface(&acc);
    obj.ComputeBounds();
    ObjectGroup group;
    group.AddObject(&obj);

    PointLight light;
    TraceContext cxt = SlCameraContext(&group);
    TASK_CHECK(!light.BuildDeepShadowMap(cxt, 4, 10));
  }

  // point cloud target is rejected
  {
    PointCloud cloud;
    BVHAccelerator acc;
    acc.SetPrimitiveSet(&cloud);
    ObjectInstance obj;
    obj.SetSurface(&acc);
    obj.ComputeBounds();
    ObjectGroup group;
    group.AddObject(&obj);

    PointLight light;
    TraceContext cxt = SlCameraContext(&group);
    TASK_CHECK(!light.BuildDeepShadowMap(cxt, 4, 10));
  }

  // volume target is rejected
  {
    Volume volume;
    ObjectInstance obj;
    obj.SetVolume(&volume);
    obj.ComputeBounds();
    ObjectGroup group;
    group.AddObject(&obj);

    PointLight light;
    TraceContext cxt = SlCameraContext(&group);
    TASK_CHECK(!light.BuildDeepShadowMap(cxt, 4, 10));
  }

  // shader with non-finite Os is rejected
  {
    Sheet sheet;
    ObjectGroup group;
    make_tri_sheet(&sheet.mesh, -1);
    if (setup_sheet(&sheet, false, 0.5f, &group))
      return 1;
    sheet.shader.SetOpacity(std::numeric_limits<float>::quiet_NaN());
    if (build_top(&group))
      return 1;

    PointLight light;
    TraceContext cxt = SlCameraContext(&group);
    TASK_CHECK(!light.BuildDeepShadowMap(cxt, 4, 10));
    TASK_CHECK(!light.HasDeepShadowMap());
  }
  return 0;
}

int capture_uses_context_time()
{
  Sheet sheets[2];
  ObjectGroup group;

  // sheet A: static triangle at z=-1; instance transform moves it to z=-2
  // at time 0.5
  make_tri_sheet(&sheets[0].mesh, -1);
  if (setup_sheet(&sheets[0], false, 0.5f, &group))
    return 1;
  sheets[0].object.SetTranslate(0, 0, 0, 0);
  sheets[0].object.SetTranslate(0, 0, -2, 1);

  // sheet B: triangle at z=-10 with point velocity (0,0,8); it sits at
  // z=-6 at time 0.5
  make_tri_sheet(&sheets[1].mesh, -10);
  sheets[1].mesh.AddPointVelocity();
  for (int i = 0; i < 3; i++)
    sheets[1].mesh.SetPointVelocity(i, Vector(0, 0, 8));
  sheets[1].mesh.ComputeBounds();
  if (setup_sheet(&sheets[1], false, 0.5f, &group))
    return 1;

  if (build_top(&group))
    return 1;

  PointLight light;
  TraceContext cxt = SlCameraContext(&group);
  cxt.time = 0.5;
  TASK_CHECK(light.BuildDeepShadowMap(cxt, 4, 10));

  // breakpoints at 2*1.0607 and 6*1.0607 along the -Z neighbour rays
  Real vis = -1;
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -1.5), 0, &vis));
  TASK_CHECK(close_to(vis, 1.0));
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -3), 0, &vis));
  TASK_CHECK(close_to(vis, 0.5));
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -7), 0, &vis));
  TASK_CHECK(close_to(vis, 0.25));

  // the cache only answers for the time it was built with
  TraceContext cxt0 = SlCameraContext(&group);
  TASK_CHECK(!light.LookupShadowVisibility(cxt0, Vector(0, 0, -3), 0, &vis));
  return 0;
}

int capture_scaled_instance()
{
  Sheet sheet;
  ObjectGroup group;
  make_tri_sheet(&sheet.mesh, -1);
  if (setup_sheet(&sheet, false, 0.5f, &group))
    return 1;
  sheet.object.SetScale(2, 2, 2, 0);
  if (build_top(&group))
    return 1;

  PointLight light;
  TraceContext cxt = SlCameraContext(&group);
  TASK_CHECK(light.BuildDeepShadowMap(cxt, 4, 10));

  // with the instance scaled by 2 the sheet sits at z=-2; the stored
  // distance must be the world-space radial distance 2*1.0607, unaffected
  // by the object-space direction length
  Real vis = -1;
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -1.5), 0, &vis));
  TASK_CHECK(close_to(vis, 1.0));
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -3), 0, &vis));
  TASK_CHECK(close_to(vis, 0.5));
  return 0;
}

int performance_bound_with_backend(bool use_grid)
{
  enum { NX = 8, NY = 8, NZ = 16 };
  const int ntris = NX * NY * NZ; // 1024 spatially separated triangles

  Sheet sheet;
  ObjectGroup group;

  Mesh *mesh = &sheet.mesh;
  mesh->SetPointCount(3 * ntris);
  mesh->SetFaceCount(ntris);
  mesh->AddPointPosition();
  mesh->AddFaceIndices();

  int f = 0;
  for (int iz = 0; iz < NZ; iz++) {
    for (int iy = 0; iy < NY; iy++) {
      for (int ix = 0; ix < NX; ix++, f++) {
        const Vector c((ix - (NX - 1) / 2.0) * 4,
                       (iy - (NY - 1) / 2.0) * 4,
                       (iz - (NZ - 1) / 2.0) * 4);
        mesh->SetPointPosition(3 * f + 0, c + Vector( 0.5,  0.0,  0.2));
        mesh->SetPointPosition(3 * f + 1, c + Vector(-0.3,  0.4, -0.1));
        mesh->SetPointPosition(3 * f + 2, c + Vector(-0.2, -0.4,  0.3));
        mesh->SetFaceIndices(f, Index3(3 * f, 3 * f + 1, 3 * f + 2));
      }
    }
  }
  mesh->ComputeNormals();
  mesh->ComputeBounds();

  if (setup_sheet(&sheet, use_grid, 0.5f, &group))
    return 1;
  if (build_top(&group))
    return 1;

  PointLight light;
  TraceContext cxt = SlCameraContext(&group);

  const int res = 8;
  TriResetRayIntersectCount();
  TASK_CHECK(light.BuildDeepShadowMap(cxt, res, 200));
  const unsigned long long tests = TriGetRayIntersectCount();

  const unsigned long long rays = 6ULL * res * res;
  TASK_CHECK(tests > 0);
  // the candidate traversal must prune: far below one triangle test per
  // ray per triangle
  TASK_CHECK(tests < rays * ntris / 8);
  return 0;
}

int capture_performance_bound()
{
  if (performance_bound_with_backend(false))
    return 1;
  if (performance_bound_with_backend(true))
    return 1;
  return 0;
}

// ----------------------------------------------------------------- lookup

int lookup_neighbor_depths()
{
  Sheet left, right;
  ObjectGroup group;

  // opaque half-planes at different depths: x<=0 at z=-1, x>=0 at z=-3
  make_single_triangle(&left.mesh,
      Vector(-20, -20, -1), Vector(0, -20, -1), Vector(0, 20, -1));
  make_single_triangle(&right.mesh,
      Vector(0, -20, -3), Vector(20, -20, -3), Vector(0, 20, -3));
  if (setup_sheet(&left, false, 1.0f, &group))
    return 1;
  if (setup_sheet(&right, false, 1.0f, &group))
    return 1;
  if (build_top(&group))
    return 1;

  PointLight light;
  TraceContext cxt = SlCameraContext(&group);
  TASK_CHECK(light.BuildDeepShadowMap(cxt, 4, 10));

  // neighbours of the -Z axis: texels (2,1),(2,2) see the near sheet at
  // 1.0607, texels (1,1),(1,2) see the far sheet at 3.1820; transmittance
  // is averaged per neighbour, never the depths
  Real vis = -1;
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -1.02), 0, &vis));
  TASK_CHECK(close_to(vis, 1.0));
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -2), 0, &vis));
  TASK_CHECK(close_to(vis, 0.5));
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -3.5), 0, &vis));
  TASK_CHECK(close_to(vis, 0.0));
  return 0;
}

int lookup_cross_face_filtering()
{
  Sheet sheet;
  ObjectGroup group;

  // small opaque triangle at z=-1, only visible near the -Z axis:
  // covers |y| <= (x+1)/2 for x <= 1 in the z=-1 plane
  make_single_triangle(&sheet.mesh,
      Vector(1, 1, -1), Vector(1, -1, -1), Vector(-1, 0, -1));
  if (setup_sheet(&sheet, false, 1.0f, &group))
    return 1;
  if (build_top(&group))
    return 1;

  PointLight light;
  TraceContext cxt = SlCameraContext(&group);
  TASK_CHECK(light.BuildDeepShadowMap(cxt, 4, 10));

  // the query direction is on the +X face at u=0.985, v=0.5, so two of
  // the four bilinear neighbours fall off the +X face and must be
  // remapped onto the -Z face, where the triangle is stored
  const Vector dir = Normalize(Vector(1, 0, -0.97));
  Real vis = -1;
  // at 1.1 the remapped neighbours' breakpoint (1.2748) is not reached
  TASK_CHECK(light.LookupShadowVisibility(cxt, dir * 1.1, 0, &vis));
  TASK_CHECK(close_to(vis, 1.0));
  // weights 0.28/0.22/0.28/0.22; the two remapped neighbours transmit 0
  TASK_CHECK(light.LookupShadowVisibility(cxt, dir * 2, 0, &vis));
  TASK_CHECK(close_to(vis, 0.56));
  return 0;
}

int lookup_mismatch_returns_false()
{
  Sheet sheet;
  ObjectGroup group;
  make_tri_sheet(&sheet.mesh, -1);
  if (setup_sheet(&sheet, false, 0.5f, &group))
    return 1;
  if (build_top(&group))
    return 1;

  PointLight light;
  TraceContext cxt = SlCameraContext(&group);
  Real vis = -1;

  // nothing built yet
  TASK_CHECK(!light.LookupShadowVisibility(cxt, Vector(0, 0, -2), 0, &vis));

  TASK_CHECK(light.BuildDeepShadowMap(cxt, 4, 10));
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -2), 0, &vis));
  TASK_CHECK(close_to(vis, 0.5));

  // different time
  TraceContext cxt_time = cxt;
  cxt_time.time = 0.5;
  TASK_CHECK(!light.LookupShadowVisibility(cxt_time, Vector(0, 0, -2), 0, &vis));

  // different group
  ObjectGroup other_group;
  TraceContext cxt_group = SlCameraContext(&other_group);
  TASK_CHECK(!light.LookupShadowVisibility(cxt_group, Vector(0, 0, -2), 0, &vis));

  // receiver outside the covered distance
  TASK_CHECK(!light.LookupShadowVisibility(cxt, Vector(0, 0, -20), 0, &vis));

  // degenerate receiver at the light position
  TASK_CHECK(!light.LookupShadowVisibility(cxt, Vector(0, 0, 0), 0, &vis));

  // cleared cache
  light.ClearDeepShadowMap();
  TASK_CHECK(!light.LookupShadowVisibility(cxt, Vector(0, 0, -2), 0, &vis));
  return 0;
}

int lookup_concurrent_readonly()
{
  Sheet sheet;
  ObjectGroup group;
  make_tri_sheet(&sheet.mesh, -1);
  if (setup_sheet(&sheet, false, 0.5f, &group))
    return 1;
  if (build_top(&group))
    return 1;

  PointLight light;
  TraceContext cxt = SlCameraContext(&group);
  TASK_CHECK(light.BuildDeepShadowMap(cxt, 4, 10));

  // a built cache allows read-only queries from many threads at once
  const PointLight *clight = &light;
  const TraceContext *ccxt = &cxt;
  const int nthreads = 4;
  std::vector<std::thread> workers;
  std::vector<int> failures(nthreads, 0);
  for (int t = 0; t < nthreads; t++) {
    workers.push_back(std::thread([clight, ccxt, t, &failures]() {
      for (int i = 0; i < 200; i++) {
        const Real d = 0.5 + 0.005 * i;
        Real vis = -1;
        if (!clight->LookupShadowVisibility(*ccxt, Vector(0, 0, -d), 0, &vis))
          failures[t]++;
        const Real expect = d < 1.0606 ? 1.0 : 0.5;
        if (!close_to(vis, expect))
          failures[t]++;
      }
    }));
  }
  for (int t = 0; t < nthreads; t++)
    workers[t].join();
  for (int t = 0; t < nthreads; t++)
    TASK_CHECK(failures[t] == 0);
  return 0;
}

int lookup_transform_invalidates_cache()
{
  Sheet sheet;
  ObjectGroup group;
  make_tri_sheet(&sheet.mesh, -1);
  if (setup_sheet(&sheet, false, 0.5f, &group))
    return 1;
  if (build_top(&group))
    return 1;

  PointLight light;
  Light *base = &light;
  TraceContext cxt = SlCameraContext(&group);
  Real vis = -1;

  TASK_CHECK(light.BuildDeepShadowMap(cxt, 2, 10));
  TASK_CHECK(light.LookupShadowVisibility(cxt, Vector(0, 0, -2), 0, &vis));

  // every transform entry point on the Light base must invalidate
  base->SetTranslate(0, 0, 0, 0);
  TASK_CHECK(!light.LookupShadowVisibility(cxt, Vector(0, 0, -2), 0, &vis));

  TASK_CHECK(light.BuildDeepShadowMap(cxt, 2, 10));
  base->SetRotate(0, 0, 0, 0);
  TASK_CHECK(!light.LookupShadowVisibility(cxt, Vector(0, 0, -2), 0, &vis));

  TASK_CHECK(light.BuildDeepShadowMap(cxt, 2, 10));
  base->SetScale(1, 1, 1, 0);
  TASK_CHECK(!light.LookupShadowVisibility(cxt, Vector(0, 0, -2), 0, &vis));

  TASK_CHECK(light.BuildDeepShadowMap(cxt, 2, 10));
  base->SetTransformOrder(ORDER_SRT);
  TASK_CHECK(!light.LookupShadowVisibility(cxt, Vector(0, 0, -2), 0, &vis));

  TASK_CHECK(light.BuildDeepShadowMap(cxt, 2, 10));
  base->SetRotateOrder(ORDER_XYZ);
  TASK_CHECK(!light.LookupShadowVisibility(cxt, Vector(0, 0, -2), 0, &vis));
  return 0;
}

// --------------------------------------------------------------- transport

struct TwoSheetScene {
  Sheet sheets[2];
  ObjectGroup group;
  ObjectInstance receiver;
  PointLight light;
  LightSample sample;
  SurfaceInput in;
};

int setup_two_sheet_scene(TwoSheetScene *scene)
{
  make_tri_sheet(&scene->sheets[0].mesh, -1);
  make_tri_sheet(&scene->sheets[1].mesh, -2);
  for (int i = 0; i < 2; i++) {
    if (setup_sheet(&scene->sheets[i], false, 0.5f, &scene->group))
      return 1;
  }
  if (build_top(&scene->group))
    return 1;

  scene->receiver.SetShadowTarget(&scene->group);

  scene->light.SetColor(1, 1, 1);
  scene->light.SetIntensity(1);
  scene->light.Preprocess();
  scene->light.GetSamples(&scene->sample, 1);

  scene->in.shaded_object = &scene->receiver;
  scene->in.P = Vector(0, 0, -3);
  scene->in.N = Vector(0, 0, 1);
  return 0;
}

int transport_illuminance_with_cache_and_fallback()
{
  TwoSheetScene scene;
  if (setup_two_sheet_scene(&scene))
    return 1;

  TraceContext cxt = SlCameraContext(&scene.group);
  LightOutput out;
  const double angle = 1.5707963267948966;

  // no cache: the shadow ray only accounts for the nearest sheet
  TASK_CHECK(SlIlluminance(&cxt, &scene.sample, &scene.in.P, &scene.in.N,
      angle, &scene.in, &out));
  TASK_CHECK(std::fabs(out.Cl.r - 0.5) < 1e-6);

  // with the cache both sheets attenuate: 0.5 * 0.5
  TASK_CHECK(scene.light.BuildDeepShadowMap(cxt, 4, 10));
  TASK_CHECK(SlIlluminance(&cxt, &scene.sample, &scene.in.P, &scene.in.N,
      angle, &scene.in, &out));
  TASK_CHECK(std::fabs(out.Cl.r - 0.25) < 1e-6);

  // clearing the cache restores the original shadow ray path
  scene.light.ClearDeepShadowMap();
  TASK_CHECK(SlIlluminance(&cxt, &scene.sample, &scene.in.P, &scene.in.N,
      angle, &scene.in, &out));
  TASK_CHECK(std::fabs(out.Cl.r - 0.5) < 1e-6);
  return 0;
}

int transport_cast_shadow_off_skips_cache()
{
  TwoSheetScene scene;
  if (setup_two_sheet_scene(&scene))
    return 1;

  TraceContext cxt = SlCameraContext(&scene.group);
  TASK_CHECK(scene.light.BuildDeepShadowMap(cxt, 4, 10));

  // cast_shadow off: the cache must not be consulted either
  cxt.cast_shadow = 0;
  LightOutput out;
  TASK_CHECK(SlIlluminance(&cxt, &scene.sample, &scene.in.P, &scene.in.N,
      1.5707963267948966, &scene.in, &out));
  TASK_CHECK(std::fabs(out.Cl.r - 1.0) < 1e-6);
  return 0;
}

int transport_no_tracing_on_cached_query()
{
  TwoSheetScene scene;
  if (setup_two_sheet_scene(&scene))
    return 1;

  TraceContext cxt = SlCameraContext(&scene.group);
  LightOutput out;
  const double angle = 1.5707963267948966;

  TASK_CHECK(scene.light.BuildDeepShadowMap(cxt, 4, 10));

  // a cached illuminance query must not trace a single triangle
  TriResetRayIntersectCount();
  TASK_CHECK(SlIlluminance(&cxt, &scene.sample, &scene.in.P, &scene.in.N,
      angle, &scene.in, &out));
  TASK_CHECK(std::fabs(out.Cl.r - 0.25) < 1e-6);
  TASK_CHECK(TriGetRayIntersectCount() == 0);

  // the fallback does trace
  scene.light.ClearDeepShadowMap();
  TriResetRayIntersectCount();
  TASK_CHECK(SlIlluminance(&cxt, &scene.sample, &scene.in.P, &scene.in.N,
      angle, &scene.in, &out));
  TASK_CHECK(TriGetRayIntersectCount() > 0);
  return 0;
}

int transport_receiver_bias()
{
  TwoSheetScene scene;
  if (setup_two_sheet_scene(&scene))
    return 1;

  TraceContext cxt = SlCameraContext(&scene.group);
  TASK_CHECK(scene.light.BuildDeepShadowMap(cxt, 4, 10));

  LightOutput out;
  const double angle = 1.5707963267948966;

  // a receiver exactly on the first sheet is not self-shadowed: the
  // query distance is reduced by the 0.0001 world-unit bias
  scene.in.P = Vector(0, 0, -1);
  TASK_CHECK(SlIlluminance(&cxt, &scene.sample, &scene.in.P, &scene.in.N,
      angle, &scene.in, &out));
  TASK_CHECK(std::fabs(out.Cl.r - 1.0) < 1e-6);

  scene.in.P = Vector(0, 0, -1.5);
  TASK_CHECK(SlIlluminance(&cxt, &scene.sample, &scene.in.P, &scene.in.N,
      angle, &scene.in, &out));
  TASK_CHECK(std::fabs(out.Cl.r - 0.5) < 1e-6);

  scene.in.P = Vector(0, 0, -2.5);
  TASK_CHECK(SlIlluminance(&cxt, &scene.sample, &scene.in.P, &scene.in.N,
      angle, &scene.in, &out));
  TASK_CHECK(std::fabs(out.Cl.r - 0.25) < 1e-6);
  return 0;
}

} // anonymous namespace

const struct task_case task_cases[] = {
  {"capture", "layered_transmittance", capture_layered_transmittance},
  {"capture", "same_distance_instances", capture_same_distance_instances},
  {"capture", "grid_duplicate_counts_once", capture_grid_duplicate_counts_once},
  {"capture", "rejects_unsupported", capture_rejects_unsupported},
  {"capture", "uses_context_time", capture_uses_context_time},
  {"capture", "scaled_instance", capture_scaled_instance},
  {"capture", "performance_bound", capture_performance_bound},
  {"lookup", "neighbor_depths", lookup_neighbor_depths},
  {"lookup", "cross_face_filtering", lookup_cross_face_filtering},
  {"lookup", "mismatch_returns_false", lookup_mismatch_returns_false},
  {"lookup", "concurrent_readonly", lookup_concurrent_readonly},
  {"lookup", "transform_invalidates_cache", lookup_transform_invalidates_cache},
  {"transport", "illuminance_with_cache_and_fallback",
      transport_illuminance_with_cache_and_fallback},
  {"transport", "cast_shadow_off_skips_cache",
      transport_cast_shadow_off_skips_cache},
  {"transport", "no_tracing_on_cached_query",
      transport_no_tracing_on_cached_query},
  {"transport", "receiver_bias", transport_receiver_bias},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
