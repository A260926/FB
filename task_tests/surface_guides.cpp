// Surface guide tests: capture / adaptive / reconstruct groups.
// Registered through task_cases/task_case_count; no main() here.

#include "task_support/check.h"

#include "fj_surface_guides.h"
#include "fj_renderer.h"
#include "fj_mesh.h"
#include "fj_bvh_accelerator.h"
#include "fj_object_instance.h"
#include "fj_object_group.h"
#include "fj_accelerator.h"
#include "fj_shader.h"
#include "fj_framebuffer.h"
#include "fj_camera.h"
#include "fj_shading.h"
#include "fj_volume.h"
#include "fj_vector.h"
#include "fj_color.h"
#include "fj_ray.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace fj;

namespace {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// Constant color shader that counts camera ray evaluations, so tests can
// verify that enabling guides does not issue extra primary rays or extra
// material calls.
class FlatShader : public Shader {
public:
  FlatShader() : calls(0), r(.25f), g(.5f), b(.75f) {}
  FlatShader(float rr, float gg, float bb) : calls(0), r(rr), g(gg), b(bb) {}

  mutable std::atomic<int> calls;
  float r, g, b;

private:
  void evaluate(const TraceContext &cxt, const SurfaceInput &,
      SurfaceOutput *out) const override
  {
    if (cxt.ray_context == CXT_CAMERA_RAY) {
      ++calls;
    }
    out->Cs = Color(r, g, b);
    out->Os = 1;
  }
};

// Shader that recursively traces reflection, refraction and shadow rays.
// The guide of the primary hit must survive all of them.
class RecursiveShader : public Shader {
public:
  RecursiveShader() : calls(0) {}
  mutable std::atomic<int> calls;

private:
  void evaluate(const TraceContext &cxt, const SurfaceInput &in,
      SurfaceOutput *out) const override
  {
    if (cxt.ray_context == CXT_CAMERA_RAY) {
      ++calls;
    }

    Color4 C;
    double t = 0;

    // reflection
    Vector R;
    SlReflect(&in.I, &in.N, &R);
    R = Normalize(R);
    TraceContext refl_cxt = SlReflectContext(&cxt, in.shaded_object);
    SlTrace(&refl_cxt, &in.P, &R, .0001, 1000, &C, &t);
    out->Cs = Color(C.r, C.g, C.b);

    // refraction
    Vector T;
    SlRefract(&in.I, &in.N, 1.4, &T);
    T = Normalize(T);
    TraceContext refr_cxt = SlRefractContext(&cxt, in.shaded_object);
    SlTrace(&refr_cxt, &in.P, &T, .0001, 1000, &C, &t);
    out->Cs.r += .5f * C.r;
    out->Cs.g += .5f * C.g;
    out->Cs.b += .5f * C.b;

    // shadow style ray straight back at the scene
    Vector up(0, 0, 1);
    TraceContext shad_cxt = SlShadowContext(&cxt, in.shaded_object);
    SlTrace(&shad_cxt, &in.P, &up, .0001, 1000, &C, &t);

    out->Os = 1;
  }
};

// Axis aligned quad in a z = const plane, facing the camera (+z normal).
// Must not be copied after init(): the accelerator points at the mesh.
struct Quad {
  Mesh mesh;
  BVHAccelerator acc;
  ObjectInstance object;

  void init(double x0, double y0, double x1, double y1, double z)
  {
    mesh.SetPointCount(4);
    mesh.SetFaceCount(2);
    mesh.AddPointPosition();
    mesh.AddFaceIndices();
    mesh.SetPointPosition(0, Vector(x0, y0, z));
    mesh.SetPointPosition(1, Vector(x1, y0, z));
    mesh.SetPointPosition(2, Vector(x1, y1, z));
    mesh.SetPointPosition(3, Vector(x0, y1, z));
    mesh.SetFaceIndices(0, Index3(0, 1, 2));
    mesh.SetFaceIndices(1, Index3(0, 2, 3));
    mesh.ComputeNormals();
    mesh.ComputeBounds();
    acc.SetPrimitiveSet(&mesh);
    acc.Build();
    object.SetSurface(&acc);
    object.ComputeBounds();
  }

  // overrides the geometric shading normals with a constant custom one
  void set_custom_normal(const Vector &n)
  {
    for (int i = 0; i < 4; i++) {
      mesh.SetPointNormal(i, n);
    }
  }
};

struct Scene {
  ObjectGroup group;
  Camera cam;
  FrameBuffer fb;
  Renderer ren;

  int build()
  {
    group.ComputeBounds();
    return const_cast<Accelerator *>(group.GetSurfaceAccelerator())->Build();
  }
};

struct RenderOptions {
  RenderOptions() :
      xres(16), yres(16),
      sampler(RENDERER_FIXED_GRID_SAMPLER),
      xsamples(1), ysamples(1),
      max_subd(4),
      tile(64), threads(1),
      filter_width(1),
      color_threshold(1e9f),
      depth_range(1e9), normal_error(2),
      use_region(false),
      rxmin(0), rymin(0), rxmax(0), rymax(0)
  {}

  int xres, yres;
  int sampler;
  int xsamples, ysamples;
  int max_subd;
  int tile, threads;
  double filter_width;
  float color_threshold;
  double depth_range, normal_error;
  bool use_region;
  int rxmin, rymin, rxmax, rymax;
};

int render(Scene &scene, SurfaceGuideBuffer *guides, const RenderOptions &opt)
{
  Renderer &ren = scene.ren;
  ren.SetCamera(&scene.cam);
  ren.SetFrameBuffers(&scene.fb);
  ren.SetTargetObjects(&scene.group);
  ren.SetResolution(opt.xres, opt.yres);
  if (opt.use_region) {
    ren.SetRenderRegion(opt.rxmin, opt.rymin, opt.rxmax, opt.rymax);
  }
  ren.SetTileSize(opt.tile, opt.tile);
  ren.SetSampleJitter(0);
  ren.SetSampleTimeRange(0, 0);
  ren.SetFilterWidth(opt.filter_width, opt.filter_width);
  ren.SetFrameReportCallback(NULL, NULL, NULL, NULL);
  ren.SetTileReportCallback(NULL, NULL, NULL, NULL);
  ren.SetPixelSamples(opt.xsamples, opt.ysamples);
  ren.SetMaxSubdivision(opt.max_subd);
  ren.SetSubdivisionThreshold(opt.color_threshold);
  ren.SetSamplerType(opt.sampler);
  ren.SetThreadCount(opt.threads);
  ren.SetGuideThresholds(opt.depth_range, opt.normal_error);
  ren.SetSurfaceGuides(guides);
  return ren.RenderScene();
}

// ray direction for the same uv convention the samplers use
Vector ray_dir_for_uv(const Camera &cam, double u, double v)
{
  Ray ray;
  cam.GetRay(Vector2(u, v), 0, &ray);
  return ray.dir;
}

// analytic distance from the camera to the plane z = zplane along dir
double plane_t(const Vector &dir, double zplane)
{
  return zplane / dir.z;
}

bool vec_near(const Vector &v, double x, double y, double z, double tol)
{
  return std::fabs(v.x - x) < tol &&
         std::fabs(v.y - y) < tol &&
         std::fabs(v.z - z) < tol;
}

bool guide_is_zero(const SurfaceGuide &g)
{
  return g.id == 0 && g.depth == 0 &&
      g.normal.x == 0 && g.normal.y == 0 && g.normal.z == 0 &&
      g.coverage == 0;
}

// ---------------------------------------------------------------------------
// capture group
// ---------------------------------------------------------------------------

int capture_ids_defaults_and_same_color()
{
  // ObjectInstance guide id API
  ObjectInstance probe;
  TASK_CHECK(probe.GetGuideID() == 1);
  probe.SetGuideID(42);
  TASK_CHECK(probe.GetGuideID() == 42);

  bool threw = false;
  try {
    probe.SetGuideID(0);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(probe.GetGuideID() == 42);

  // two adjacent quads with identical color but different labels
  Scene scene;
  FlatShader flat(.4f, .6f, .8f);
  Quad left, right;
  left.init(-2, -2, 0, 2, -3);
  right.init(0, -2, 2, 2, -3);
  left.object.SetGuideID(3);
  right.object.SetGuideID(7);
  left.object.SetShader(&flat, 0);
  right.object.SetShader(&flat, 0);
  scene.group.AddObject(&left.object);
  scene.group.AddObject(&right.object);
  TASK_CHECK(scene.build() == 0);

  SurfaceGuideBuffer guides;
  RenderOptions opt; // 16x16, fixed, 1x1 samples, filter width 1
  TASK_CHECK(render(scene, &guides, opt) == 0);

  TASK_CHECK(guides.GetWidth() == 16);
  TASK_CHECK(guides.GetHeight() == 16);
  TASK_CHECK(!guides.IsEmpty());

  // every camera ray was shaded exactly once: guides reuse the same
  // intersection and do not re-run materials
  TASK_CHECK(flat.calls.load() == 16 * 16);

  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      const SurfaceGuide g = guides.GetPixel(x, y);
      const uint32_t expect_id = x < 8 ? 3 : 7;
      TASK_CHECK(g.id == expect_id);
      TASK_CHECK(std::fabs(g.coverage - 1) < 1e-12);

      const double u = (x + .5) / 16.;
      const double v = 1 - (y + .5) / 16.;
      const Vector dir = ray_dir_for_uv(scene.cam, u, v);
      TASK_CHECK(std::fabs(g.depth - plane_t(dir, -3)) < 1e-9);
      TASK_CHECK(vec_near(g.normal, 0, 0, 1, 1e-9));
    }
  }

  // identical colors on both sides of the label boundary
  const Color4 c0 = scene.fb.GetColor(4, 8);
  const Color4 c1 = scene.fb.GetColor(12, 8);
  TASK_CHECK(std::fabs(c0.r - .4) < 1e-6 && std::fabs(c0.g - .6) < 1e-6 &&
      std::fabs(c0.b - .8) < 1e-6 && std::fabs(c0.a - 1) < 1e-6);
  TASK_CHECK(std::fabs(c1.r - c0.r) < 1e-6 && std::fabs(c1.g - c0.g) < 1e-6 &&
      std::fabs(c1.b - c0.b) < 1e-6);

  return 0;
}

int capture_background_boundary()
{
  Scene scene;
  FlatShader flat;
  Quad quad;
  // small quad in the center of the frame
  quad.init(-.35, -.35, .35, .35, -3);
  quad.object.SetGuideID(5);
  quad.object.SetShader(&flat, 0);
  scene.group.AddObject(&quad.object);
  TASK_CHECK(scene.build() == 0);

  SurfaceGuideBuffer guides;
  RenderOptions opt; // 16x16, fixed, 1x1, filter 1
  TASK_CHECK(render(scene, &guides, opt) == 0);

  // deep background: everything zero
  TASK_CHECK(guide_is_zero(guides.GetPixel(0, 0)));
  TASK_CHECK(guide_is_zero(guides.GetPixel(15, 0)));
  TASK_CHECK(guide_is_zero(guides.GetPixel(0, 15)));
  TASK_CHECK(guide_is_zero(guides.GetPixel(15, 15)));

  // out-of-bounds reads return zeros
  TASK_CHECK(guide_is_zero(guides.GetPixel(-1, 8)));
  TASK_CHECK(guide_is_zero(guides.GetPixel(16, 8)));
  TASK_CHECK(guide_is_zero(guides.GetPixel(8, 16)));

  // center of the quad
  const SurfaceGuide center = guides.GetPixel(8, 8);
  TASK_CHECK(center.id == 5);
  TASK_CHECK(std::fabs(center.coverage - 1) < 1e-12);
  {
    const Vector dir = ray_dir_for_uv(scene.cam, 8.5 / 16., 1 - 8.5 / 16.);
    TASK_CHECK(std::fabs(center.depth - plane_t(dir, -3)) < 1e-9);
    TASK_CHECK(vec_near(center.normal, 0, 0, 1, 1e-9));
  }

  // pixels fully inside / fully outside
  TASK_CHECK(guides.GetPixel(5, 5).id == 5);
  TASK_CHECK(std::fabs(guides.GetPixel(5, 5).coverage - 1) < 1e-12);
  TASK_CHECK(guide_is_zero(guides.GetPixel(3, 8)));

  // id 0 pixels must carry zero depth and zero normal
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      const SurfaceGuide g = guides.GetPixel(x, y);
      if (g.id == 0) {
        TASK_CHECK(g.depth == 0);
        TASK_CHECK(g.normal.x == 0 && g.normal.y == 0 && g.normal.z == 0);
      } else {
        TASK_CHECK(g.id == 5);
        TASK_CHECK(g.depth > 0);
      }
      TASK_CHECK(g.coverage >= 0 && g.coverage <= 1);
    }
  }

  return 0;
}

int capture_depth_normal_exact()
{
  Scene scene;
  FlatShader flat;
  Quad quad;
  quad.init(-2, -2, 2, 2, -3);
  quad.object.SetGuideID(9);
  quad.object.SetShader(&flat, 0);
  // constant custom shading normal, tilted away from the geometric normal
  const Vector tilted = Normalize(Vector(.5, 0, 1));
  quad.set_custom_normal(tilted);
  scene.group.AddObject(&quad.object);
  TASK_CHECK(scene.build() == 0);

  SurfaceGuideBuffer guides;
  RenderOptions opt;
  opt.xres = 8;
  opt.yres = 8;
  TASK_CHECK(render(scene, &guides, opt) == 0);

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      const SurfaceGuide g = guides.GetPixel(x, y);
      TASK_CHECK(g.id == 9);
      TASK_CHECK(std::fabs(g.coverage - 1) < 1e-12);

      const double u = (x + .5) / 8.;
      const double v = 1 - (y + .5) / 8.;
      const Vector dir = ray_dir_for_uv(scene.cam, u, v);
      // primary ray distance to the plane, taken from the same intersection
      TASK_CHECK(std::fabs(g.depth - plane_t(dir, -3)) < 1e-9);
      // the shading normal, not the geometric one
      TASK_CHECK(vec_near(g.normal, tilted.x, tilted.y, tilted.z, 1e-9));
    }
  }

  return 0;
}

int capture_recursive_material()
{
  Scene scene;
  RecursiveShader mirror_shader;
  FlatShader red(1, 0, 0);
  FlatShader green(0, 1, 0);

  Quad mirror, back, behind;
  mirror.init(-2, -2, 2, 2, -2);   // primary surface
  back.init(-2, -2, 2, 2, 5);      // seen through reflection
  behind.init(-2, -2, 2, 2, -6);   // seen through refraction

  mirror.object.SetGuideID(5);
  back.object.SetGuideID(9);
  behind.object.SetGuideID(11);

  mirror.object.SetShader(&mirror_shader, 0);
  back.object.SetShader(&red, 0);
  behind.object.SetShader(&green, 0);

  scene.group.AddObject(&mirror.object);
  scene.group.AddObject(&back.object);
  scene.group.AddObject(&behind.object);
  TASK_CHECK(scene.build() == 0);

  mirror.object.SetReflectTarget(&scene.group);
  mirror.object.SetRefractTarget(&scene.group);
  mirror.object.SetShadowTarget(&scene.group);

  SurfaceGuideBuffer guides;
  RenderOptions opt;
  opt.xres = 8;
  opt.yres = 8;
  TASK_CHECK(render(scene, &guides, opt) == 0);

  // recursion must not add primary rays: exactly one camera ray per sample
  TASK_CHECK(mirror_shader.calls.load() == 8 * 8);
  // the other shaders were only reached through recursive rays
  TASK_CHECK(red.calls.load() == 0);
  TASK_CHECK(green.calls.load() == 0);

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      const SurfaceGuide g = guides.GetPixel(x, y);
      // the guide describes the primary hit only: the mirror, never the
      // objects seen through reflection/refraction/shadow rays
      TASK_CHECK(g.id == 5);
      TASK_CHECK(std::fabs(g.coverage - 1) < 1e-12);
      const double u = (x + .5) / 8.;
      const double v = 1 - (y + .5) / 8.;
      const Vector dir = ray_dir_for_uv(scene.cam, u, v);
      TASK_CHECK(std::fabs(g.depth - plane_t(dir, -2)) < 1e-9);
      TASK_CHECK(vec_near(g.normal, 0, 0, 1, 1e-9));
    }
  }

  // sanity: the recursion really happened and shows up in the beauty
  const Color4 c = scene.fb.GetColor(4, 4);
  TASK_CHECK(std::fabs(c.r - 1) < 1e-6);   // red via reflection
  TASK_CHECK(std::fabs(c.g - .5) < 1e-6);  // green via refraction

  return 0;
}

int capture_region_render()
{
  Scene scene;
  FlatShader flat;
  Quad quad;
  quad.init(-2, -2, 2, 2, -3);
  quad.object.SetGuideID(4);
  quad.object.SetShader(&flat, 0);
  scene.group.AddObject(&quad.object);
  TASK_CHECK(scene.build() == 0);

  SurfaceGuideBuffer guides;
  RenderOptions opt;
  opt.use_region = true;
  opt.rxmin = 4; opt.rymin = 6;
  opt.rxmax = 12; opt.rymax = 10;
  TASK_CHECK(render(scene, &guides, opt) == 0);

  // the buffer still covers the whole frame
  TASK_CHECK(guides.GetWidth() == 16);
  TASK_CHECK(guides.GetHeight() == 16);

  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      const SurfaceGuide g = guides.GetPixel(x, y);
      const bool inside = x >= 4 && x < 12 && y >= 6 && y < 10;
      if (inside) {
        TASK_CHECK(g.id == 4);
        TASK_CHECK(std::fabs(g.coverage - 1) < 1e-12);
      } else {
        // outside the render region everything stays zero
        TASK_CHECK(guide_is_zero(g));
      }
    }
  }

  return 0;
}

int capture_volume_beauty()
{
  Scene scene;
  FlatShader flat(.2f, .4f, .6f);
  FlatShader volume_shader(0, 0, 1);

  Quad quad;
  quad.init(-2, -2, 2, 2, -5);
  quad.object.SetGuideID(6);
  quad.object.SetShader(&flat, 0);

  // constant density volume box between the camera and the quad
  Volume volume;
  volume.Resize(2, 2, 2);
  volume.SetBounds(Box(Vector(-2, -2, -3.5), Vector(2, 2, -1)));
  for (int k = 0; k < 2; k++)
    for (int j = 0; j < 2; j++)
      for (int i = 0; i < 2; i++)
        volume.SetValue(i, j, k, 1.5f);

  ObjectInstance vobj;
  vobj.SetVolume(&volume);
  vobj.SetShader(&volume_shader, 0);
  vobj.ComputeBounds();

  scene.group.AddObject(&quad.object);
  scene.group.AddObject(&vobj);
  TASK_CHECK(scene.build() == 0);

  SurfaceGuideBuffer guides;
  RenderOptions opt;
  opt.xres = 8;
  opt.yres = 8;
  TASK_CHECK(render(scene, &guides, opt) == 0);

  // the volume does not participate in the guides: the surface behind it
  // is reported as if the volume was not there
  const SurfaceGuide g = guides.GetPixel(4, 4);
  TASK_CHECK(g.id == 6);
  TASK_CHECK(std::fabs(g.coverage - 1) < 1e-12);
  {
    const Vector dir = ray_dir_for_uv(scene.cam, 4.5 / 8., 1 - 4.5 / 8.);
    TASK_CHECK(std::fabs(g.depth - plane_t(dir, -5)) < 1e-9);
    TASK_CHECK(vec_near(g.normal, 0, 0, 1, 1e-9));
  }

  // beauty still renders the volume: colors differ from the no-volume scene
  Scene scene2;
  Quad quad2;
  quad2.init(-2, -2, 2, 2, -5);
  quad2.object.SetGuideID(6);
  quad2.object.SetShader(&flat, 0);
  scene2.group.AddObject(&quad2.object);
  TASK_CHECK(scene2.build() == 0);
  TASK_CHECK(render(scene2, NULL, opt) == 0);

  const Color4 with_vol = scene.fb.GetColor(4, 4);
  const Color4 without_vol = scene2.fb.GetColor(4, 4);
  TASK_CHECK(std::fabs(with_vol.b - without_vol.b) > .01);

  return 0;
}

int capture_thresholds_and_toggle()
{
  Renderer ren;
  // documented defaults
  TASK_CHECK(ren.guide_depth_range_ == .1);
  TASK_CHECK(ren.guide_normal_error_ == .05);

  ren.SetGuideThresholds(.5, 1.5);
  TASK_CHECK(ren.guide_depth_range_ == .5);
  TASK_CHECK(ren.guide_normal_error_ == 1.5);

  // invalid values keep the previous thresholds
  ren.SetGuideThresholds(-1, .1);
  TASK_CHECK(ren.guide_depth_range_ == .5 && ren.guide_normal_error_ == 1.5);
  ren.SetGuideThresholds(0, .1);
  TASK_CHECK(ren.guide_depth_range_ == .5 && ren.guide_normal_error_ == 1.5);
  ren.SetGuideThresholds(.2, 0);
  TASK_CHECK(ren.guide_depth_range_ == .5 && ren.guide_normal_error_ == 1.5);
  ren.SetGuideThresholds(.2, -3);
  TASK_CHECK(ren.guide_depth_range_ == .5 && ren.guide_normal_error_ == 1.5);
  ren.SetGuideThresholds(.2, 2.000001);
  TASK_CHECK(ren.guide_depth_range_ == .5 && ren.guide_normal_error_ == 1.5);
  ren.SetGuideThresholds(std::numeric_limits<double>::quiet_NaN(), .1);
  TASK_CHECK(ren.guide_depth_range_ == .5 && ren.guide_normal_error_ == 1.5);
  ren.SetGuideThresholds(std::numeric_limits<double>::infinity(), .1);
  TASK_CHECK(ren.guide_depth_range_ == .5 && ren.guide_normal_error_ == 1.5);
  ren.SetGuideThresholds(.2, std::numeric_limits<double>::infinity());
  TASK_CHECK(ren.guide_depth_range_ == .5 && ren.guide_normal_error_ == 1.5);

  // boundary value 2 is allowed for the normal error
  ren.SetGuideThresholds(.3, 2);
  TASK_CHECK(ren.guide_depth_range_ == .3);
  TASK_CHECK(ren.guide_normal_error_ == 2.);

  // NULL disables the guide output
  Scene scene;
  FlatShader flat;
  Quad quad;
  quad.init(-2, -2, 2, 2, -3);
  quad.object.SetGuideID(4);
  quad.object.SetShader(&flat, 0);
  scene.group.AddObject(&quad.object);
  TASK_CHECK(scene.build() == 0);

  SurfaceGuideBuffer guides;
  RenderOptions opt;
  opt.xres = 8;
  opt.yres = 8;
  TASK_CHECK(render(scene, &guides, opt) == 0);
  TASK_CHECK(guides.GetWidth() == 8 && guides.GetHeight() == 8);
  TASK_CHECK(guides.GetPixel(4, 4).id == 4);

  // a fresh buffer stays empty when guides are disabled
  SurfaceGuideBuffer off_guides;
  TASK_CHECK(render(scene, NULL, opt) == 0);
  TASK_CHECK(off_guides.IsEmpty());

  // every render resets the buffer at the current resolution
  RenderOptions smaller;
  smaller.xres = 5;
  smaller.yres = 3;
  TASK_CHECK(render(scene, &guides, smaller) == 0);
  TASK_CHECK(guides.GetWidth() == 5 && guides.GetHeight() == 3);
  TASK_CHECK(guides.GetPixel(2, 1).id == 4);

  return 0;
}

// ---------------------------------------------------------------------------
// adaptive group
// ---------------------------------------------------------------------------

int adaptive_flat_plane_ray_budget()
{
  Scene scene;
  FlatShader flat;
  Quad quad;
  quad.init(-2, -2, 2, 2, -3);
  quad.object.SetGuideID(4);
  quad.object.SetShader(&flat, 0);
  scene.group.AddObject(&quad.object);
  TASK_CHECK(scene.build() == 0);

  SurfaceGuideBuffer guides;
  RenderOptions opt;
  opt.xres = 32;
  opt.yres = 32;
  opt.tile = 32;
  opt.sampler = RENDERER_ADAPTIVE_GRID_SAMPLER;
  opt.max_subd = 4;
  // large thresholds: nothing should trigger subdivision on a flat,
  // single-label plane
  opt.color_threshold = 1e9f;
  opt.depth_range = 1e9;
  opt.normal_error = 2;
  TASK_CHECK(render(scene, &guides, opt) == 0);

  const int div = 1 << 4; // 2^max_subd sample divisions per pixel
  const long full_grid = (32L * div + 1) * (32L * div + 1);
  const long budget = full_grid / 8;
  const int rays = flat.calls.load();
  std::printf("adaptive_flat_plane_ray_budget primary_rays=%d full_grid=%ld budget=%ld\n",
      rays, full_grid, budget);
  TASK_CHECK(rays > 0);
  TASK_CHECK(rays <= budget);

  // interior pixels are filled by interpolation with the same label
  const SurfaceGuide g = guides.GetPixel(16, 16);
  TASK_CHECK(g.id == 4);
  TASK_CHECK(std::fabs(g.coverage - 1) < 1e-12);
  TASK_CHECK(vec_near(g.normal, 0, 0, 1, 1e-6));
  {
    const Vector dir = ray_dir_for_uv(scene.cam, 16.5 / 32., 1 - 16.5 / 32.);
    TASK_CHECK(std::fabs(g.depth - plane_t(dir, -3)) < 1e-3);
  }

  return 0;
}

int adaptive_label_boundary_subdivides()
{
  // two quads with identical color, depth and normals, different labels.
  // the shared edge sits at x = .02, off the adaptive sample grid, so
  // sample rays never land exactly on the geometric crack between the
  // two meshes.
  Scene mixed;
  FlatShader flat_m(.5f, .5f, .5f);
  Quad left_m, right_m;
  left_m.init(-2, -2, .02, 2, -3);
  right_m.init(.02, -2, 2, 2, -3);
  left_m.object.SetGuideID(3);
  right_m.object.SetGuideID(7);
  left_m.object.SetShader(&flat_m, 0);
  right_m.object.SetShader(&flat_m, 0);
  mixed.group.AddObject(&left_m.object);
  mixed.group.AddObject(&right_m.object);
  TASK_CHECK(mixed.build() == 0);

  // same scene with a single shared label
  Scene single;
  FlatShader flat_s(.5f, .5f, .5f);
  Quad left_s, right_s;
  left_s.init(-2, -2, .02, 2, -3);
  right_s.init(.02, -2, 2, 2, -3);
  left_s.object.SetGuideID(3);
  right_s.object.SetGuideID(3);
  left_s.object.SetShader(&flat_s, 0);
  right_s.object.SetShader(&flat_s, 0);
  single.group.AddObject(&left_s.object);
  single.group.AddObject(&right_s.object);
  TASK_CHECK(single.build() == 0);

  RenderOptions opt;
  opt.xres = 32;
  opt.yres = 32;
  opt.tile = 32;
  opt.sampler = RENDERER_ADAPTIVE_GRID_SAMPLER;
  opt.max_subd = 4;
  // color, depth and normal criteria are all silenced; only the label
  // difference can trigger subdivision
  opt.color_threshold = 1e9f;
  opt.depth_range = 1e9;
  opt.normal_error = 2;

  SurfaceGuideBuffer guides_mixed, guides_single;
  TASK_CHECK(render(mixed, &guides_mixed, opt) == 0);
  TASK_CHECK(render(single, &guides_single, opt) == 0);

  const int rays_mixed = flat_m.calls.load();
  const int rays_single = flat_s.calls.load();
  std::printf("adaptive_label_boundary_subdivides rays_mixed=%d rays_single=%d\n",
      rays_mixed, rays_single);
  // the label boundary forces extra subdivision even though the two sides
  // are shaded with the identical color
  TASK_CHECK(rays_mixed > rays_single + 200);

  // deep inside each side the labels are right, and no new label appears
  for (int y = 0; y < 32; y++) {
    TASK_CHECK(guides_mixed.GetPixel(2, y).id == 3);
    TASK_CHECK(guides_mixed.GetPixel(29, y).id == 7);
    for (int x = 0; x < 32; x++) {
      const uint32_t id = guides_mixed.GetPixel(x, y).id;
      TASK_CHECK(id == 3 || id == 7);
    }
  }

  return 0;
}

int adaptive_depth_discontinuity_subdivides()
{
  // same label, same color, same normals: only the depth jump can trigger.
  // the shared edge is off the sample grid (see the label boundary case)
  Scene scene;
  FlatShader flat(.6f, .6f, .6f);
  Quad near_q, far_q;
  near_q.init(-2, -2, .02, 2, -2);
  far_q.init(.02, -2, 2, 2, -5);
  near_q.object.SetGuideID(4);
  far_q.object.SetGuideID(4);
  near_q.object.SetShader(&flat, 0);
  far_q.object.SetShader(&flat, 0);
  scene.group.AddObject(&near_q.object);
  scene.group.AddObject(&far_q.object);
  TASK_CHECK(scene.build() == 0);

  RenderOptions opt;
  opt.xres = 32;
  opt.yres = 32;
  opt.tile = 32;
  opt.sampler = RENDERER_ADAPTIVE_GRID_SAMPLER;
  opt.max_subd = 4;
  opt.color_threshold = 1e9f;
  opt.normal_error = 2;

  SurfaceGuideBuffer guides;
  opt.depth_range = .5; // the depth jump of 3 exceeds this
  TASK_CHECK(render(scene, &guides, opt) == 0);
  const int rays_sensitive = flat.calls.load();

  flat.calls = 0;
  SurfaceGuideBuffer guides2;
  opt.depth_range = 1e9; // depth criterion silenced
  TASK_CHECK(render(scene, &guides2, opt) == 0);
  const int rays_relaxed = flat.calls.load();

  std::printf("adaptive_depth_discontinuity_subdivides rays_sensitive=%d rays_relaxed=%d\n",
      rays_sensitive, rays_relaxed);
  TASK_CHECK(rays_sensitive > rays_relaxed + 200);

  // depths on both sides are still reported correctly
  const SurfaceGuide gleft = guides.GetPixel(4, 16);
  const SurfaceGuide gright = guides.GetPixel(28, 16);
  TASK_CHECK(gleft.id == 4 && gright.id == 4);
  TASK_CHECK(gleft.depth > 1.9 && gleft.depth < 2.2);
  TASK_CHECK(gright.depth > 4.9 && gright.depth < 5.4);

  return 0;
}

int adaptive_normal_discontinuity_subdivides()
{
  // same label, same color, same depth: only the normal jump can trigger.
  // the shared edge is off the sample grid (see the label boundary case)
  Scene scene;
  FlatShader flat(.6f, .6f, .6f);
  Quad left, right;
  left.init(-2, -2, .02, 2, -3);
  right.init(.02, -2, 2, 2, -3);
  left.object.SetGuideID(4);
  right.object.SetGuideID(4);
  left.object.SetShader(&flat, 0);
  right.object.SetShader(&flat, 0);
  // tilt the right hand side shading normals by 60 degrees
  right.set_custom_normal(Normalize(Vector(1, 0, 1)));
  scene.group.AddObject(&left.object);
  scene.group.AddObject(&right.object);
  TASK_CHECK(scene.build() == 0);

  RenderOptions opt;
  opt.xres = 32;
  opt.yres = 32;
  opt.tile = 32;
  opt.sampler = RENDERER_ADAPTIVE_GRID_SAMPLER;
  opt.max_subd = 4;
  opt.color_threshold = 1e9f;
  opt.depth_range = 1e9;

  SurfaceGuideBuffer guides;
  opt.normal_error = .05; // 1 - dot = 1 - cos(45) exceeds this
  TASK_CHECK(render(scene, &guides, opt) == 0);
  const int rays_sensitive = flat.calls.load();

  flat.calls = 0;
  SurfaceGuideBuffer guides2;
  opt.normal_error = 2; // normal criterion silenced
  TASK_CHECK(render(scene, &guides2, opt) == 0);
  const int rays_relaxed = flat.calls.load();

  std::printf("adaptive_normal_discontinuity_subdivides rays_sensitive=%d rays_relaxed=%d\n",
      rays_sensitive, rays_relaxed);
  TASK_CHECK(rays_sensitive > rays_relaxed + 200);

  // the custom shading normal is reported on the right hand side
  const SurfaceGuide g = guides.GetPixel(28, 16);
  TASK_CHECK(g.id == 4);
  TASK_CHECK(vec_near(g.normal, std::sqrt(.5), 0, std::sqrt(.5), 1e-3));

  return 0;
}

int adaptive_tile_thread_determinism()
{
  // label + depth discontinuity so the adaptive sampler actually
  // subdivides; the shared edge is off the sample grid
  Scene scene;
  FlatShader flat(.3f, .5f, .7f);
  Quad left, right;
  left.init(-2, -2, .02, 2, -2.5);
  right.init(.02, -2, 2, 2, -4);
  left.object.SetGuideID(3);
  right.object.SetGuideID(7);
  left.object.SetShader(&flat, 0);
  right.object.SetShader(&flat, 0);
  scene.group.AddObject(&left.object);
  scene.group.AddObject(&right.object);
  TASK_CHECK(scene.build() == 0);

  RenderOptions base;
  base.xres = 32;
  base.yres = 32;
  base.sampler = RENDERER_ADAPTIVE_GRID_SAMPLER;
  base.max_subd = 3;
  base.filter_width = 2;
  base.color_threshold = .05f;
  base.depth_range = .1;
  base.normal_error = .05;

  const int tiles[] = {8, 16, 32, 8, 13};
  const int threads[] = {1, 1, 1, 4, 2};
  const int nconfigs = 5;

  SurfaceGuideBuffer bufs[5];
  for (int i = 0; i < nconfigs; i++) {
    RenderOptions opt = base;
    opt.tile = tiles[i];
    opt.threads = threads[i];
    TASK_CHECK(render(scene, &bufs[i], opt) == 0);
  }

  // with jitter and time sampling off, every configuration must produce
  // the same guide values within 1e-6
  for (int c = 1; c < nconfigs; c++) {
    TASK_CHECK(bufs[c].GetWidth() == 32 && bufs[c].GetHeight() == 32);
    for (int y = 0; y < 32; y++) {
      for (int x = 0; x < 32; x++) {
        const SurfaceGuide a = bufs[0].GetPixel(x, y);
        const SurfaceGuide b = bufs[c].GetPixel(x, y);
        TASK_CHECK(a.id == b.id);
        TASK_CHECK(std::fabs(a.depth - b.depth) <= 1e-6);
        TASK_CHECK(std::fabs(a.normal.x - b.normal.x) <= 1e-6);
        TASK_CHECK(std::fabs(a.normal.y - b.normal.y) <= 1e-6);
        TASK_CHECK(std::fabs(a.normal.z - b.normal.z) <= 1e-6);
        TASK_CHECK(std::fabs(a.coverage - b.coverage) <= 1e-6);
      }
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// reconstruct group
// ---------------------------------------------------------------------------

int reconstruct_voting_tie_and_coverage()
{
  Scene scene;
  FlatShader flat;
  Quad quad;
  // the right edge of the quad falls between the two sub-sample columns of
  // pixel column 2: pixels 0-1 fully covered, pixel 2 half, pixel 3 empty
  quad.init(-2, -2, .2, 2, -3);
  quad.object.SetGuideID(6);
  quad.object.SetShader(&flat, 0);
  scene.group.AddObject(&quad.object);
  TASK_CHECK(scene.build() == 0);

  SurfaceGuideBuffer guides;
  RenderOptions opt;
  opt.xres = 4;
  opt.yres = 4;
  opt.xsamples = 2;
  opt.ysamples = 2;
  opt.filter_width = 1; // margin 0: each pixel sees exactly its 4 samples
  TASK_CHECK(render(scene, &guides, opt) == 0);

  for (int y = 0; y < 4; y++) {
    // fully covered pixels
    const SurfaceGuide full = guides.GetPixel(0, y);
    TASK_CHECK(full.id == 6);
    TASK_CHECK(std::fabs(full.coverage - 1) < 1e-12);
    TASK_CHECK(vec_near(full.normal, 0, 0, 1, 1e-9));

    // depth is the weight mean of the four sub-sample distances; all four
    // gaussian weights are equal here, so it is the plain average
    double t_expect = 0;
    for (int sy = 0; sy < 2; sy++) {
      for (int sx = 0; sx < 2; sx++) {
        const double u = (sx + .5) / 8.;
        const double v = 1 - (y * 2 + sy + .5) / 8.;
        t_expect += plane_t(ray_dir_for_uv(scene.cam, u, v), -3) / 4;
      }
    }
    TASK_CHECK(std::fabs(full.depth - t_expect) < 1e-9);

    // the half covered pixel: both labels collect the same total weight,
    // the tie is won by the smaller id (background 0)
    const SurfaceGuide half = guides.GetPixel(2, y);
    TASK_CHECK(half.id == 0);
    TASK_CHECK(std::fabs(half.coverage - .5) < 1e-12);
    // the winner is the background: depth and normal are zero
    TASK_CHECK(half.depth == 0);
    TASK_CHECK(half.normal.x == 0 && half.normal.y == 0 && half.normal.z == 0);

    // fully empty pixel
    TASK_CHECK(guide_is_zero(guides.GetPixel(3, y)));
  }

  return 0;
}

int reconstruct_filter_weight_crosscheck()
{
  Scene scene;
  FlatShader flat;
  Quad quad;
  quad.init(-2, -2, 0, 2, -3); // covers the left half of the frame
  quad.object.SetGuideID(6);
  quad.object.SetShader(&flat, 0);
  scene.group.AddObject(&quad.object);
  TASK_CHECK(scene.build() == 0);

  SurfaceGuideBuffer guides;
  RenderOptions opt;
  opt.xres = 6;
  opt.yres = 6;
  opt.xsamples = 1;
  opt.ysamples = 1;
  opt.filter_width = 2; // margin 1: every pixel filters a 3x3 sample set
  TASK_CHECK(render(scene, &guides, opt) == 0);

  // independent numerical reference of the production filtering:
  // recompute the gaussian weights, the label vote, the coverage and the
  // winner-only depth average for every pixel
  for (int py = 0; py < 6; py++) {
    for (int px = 0; px < 6; px++) {
      double wsum = 0, wid = 0, tsum = 0;
      for (int sy = py - 1; sy <= py + 1; sy++) {
        for (int sx = px - 1; sx <= px + 1; sx++) {
          // sample positions may leave the frame (filter margin)
          const double u = (sx + .5) / 6.;
          const double v = 1 - (sy + .5) / 6.;
          const Vector dir = ray_dir_for_uv(scene.cam, u, v);
          const double fx = 6 * u - (px + .5);
          const double fy = 6 * (1 - v) - (py + .5);
          // gaussian with filter width 2: exp(-2 * (fx^2 + fy^2))
          const double w = std::exp(-2 * (fx * fx + fy * fy));
          wsum += w;
          const double xhit = -3 * dir.x / dir.z;
          if (xhit <= 0) {
            wid += w;
            tsum += w * plane_t(dir, -3);
          }
        }
      }

      const SurfaceGuide g = guides.GetPixel(px, py);
      const uint32_t expect_id = wid > wsum - wid ? 6 : 0;
      TASK_CHECK(g.id == expect_id);
      TASK_CHECK(std::fabs(g.coverage - wid / wsum) < 1e-9);
      if (expect_id != 0) {
        TASK_CHECK(std::fabs(g.depth - tsum / wid) < 1e-9);
        TASK_CHECK(vec_near(g.normal, 0, 0, 1, 1e-9));
      } else {
        TASK_CHECK(g.depth == 0);
        TASK_CHECK(g.normal.x == 0 && g.normal.y == 0 && g.normal.z == 0);
      }
    }
  }

  // sanity on the extremes
  TASK_CHECK(guides.GetPixel(0, 3).id == 6);
  TASK_CHECK(std::fabs(guides.GetPixel(0, 3).coverage - 1) < 1e-12);
  TASK_CHECK(guide_is_zero(guides.GetPixel(5, 3)));

  return 0;
}

int reconstruct_no_label_averaging()
{
  Scene scene;
  FlatShader flat(.5f, .5f, .5f);
  Quad left, right;
  left.init(-2, -2, .1, 2, -3);
  right.init(.1, -2, 2, 2, -3);
  left.object.SetGuideID(2);
  right.object.SetGuideID(6);
  left.object.SetShader(&flat, 0);
  right.object.SetShader(&flat, 0);
  scene.group.AddObject(&left.object);
  scene.group.AddObject(&right.object);
  TASK_CHECK(scene.build() == 0);

  SurfaceGuideBuffer guides;
  RenderOptions opt;
  opt.xres = 6;
  opt.yres = 6;
  opt.xsamples = 1;
  opt.ysamples = 1;
  opt.filter_width = 2;
  TASK_CHECK(render(scene, &guides, opt) == 0);

  for (int y = 0; y < 6; y++) {
    for (int x = 0; x < 6; x++) {
      const SurfaceGuide g = guides.GetPixel(x, y);
      // integer labels are never averaged: only real labels may appear
      // (in particular never (2 + 6) / 2 == 4)
      TASK_CHECK(g.id == 2 || g.id == 6);
      // no background anywhere in this scene
      TASK_CHECK(std::fabs(g.coverage - 1) < 1e-12);
      TASK_CHECK(g.depth > 0);
    }
    // the label boundary falls between pixel columns 2 and 3
    TASK_CHECK(guides.GetPixel(0, y).id == 2);
    TASK_CHECK(guides.GetPixel(2, y).id == 2);
    TASK_CHECK(guides.GetPixel(3, y).id == 6);
    TASK_CHECK(guides.GetPixel(5, y).id == 6);
  }

  return 0;
}

int reconstruct_beauty_preserved_fixed()
{
  Scene scene;
  FlatShader flat(.2f, .7f, .3f);
  Quad quad;
  quad.init(-2, -2, 2, 2, -3);
  quad.object.SetGuideID(4);
  quad.object.SetShader(&flat, 0);
  scene.group.AddObject(&quad.object);
  TASK_CHECK(scene.build() == 0);

  RenderOptions opt;
  opt.xres = 16;
  opt.yres = 16;
  opt.xsamples = 2;
  opt.ysamples = 2;
  opt.filter_width = 2;

  SurfaceGuideBuffer guides;
  TASK_CHECK(render(scene, &guides, opt) == 0);
  const int rays_on = flat.calls.load();

  // same scene rendered again without guides
  Scene scene2;
  Quad quad2;
  FlatShader flat2(.2f, .7f, .3f);
  quad2.init(-2, -2, 2, 2, -3);
  quad2.object.SetGuideID(4);
  quad2.object.SetShader(&flat2, 0);
  scene2.group.AddObject(&quad2.object);
  TASK_CHECK(scene2.build() == 0);
  TASK_CHECK(render(scene2, NULL, opt) == 0);
  const int rays_off = flat2.calls.load();

  // enabling guides does not issue extra primary rays or material calls
  TASK_CHECK(rays_on == rays_off);
  TASK_CHECK(rays_on > 0);

  // with the fixed sampler the beauty is identical whether guides are on
  // or off
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      const Color4 a = scene.fb.GetColor(x, y);
      const Color4 b = scene2.fb.GetColor(x, y);
      TASK_CHECK(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a);
    }
  }

  return 0;
}

} // namespace

const struct task_case task_cases[] = {
  {"capture", "ids_defaults_and_same_color", capture_ids_defaults_and_same_color},
  {"capture", "background_boundary", capture_background_boundary},
  {"capture", "depth_normal_exact", capture_depth_normal_exact},
  {"capture", "recursive_material", capture_recursive_material},
  {"capture", "region_render", capture_region_render},
  {"capture", "volume_beauty", capture_volume_beauty},
  {"capture", "thresholds_and_toggle", capture_thresholds_and_toggle},

  {"adaptive", "flat_plane_ray_budget", adaptive_flat_plane_ray_budget},
  {"adaptive", "label_boundary_subdivides", adaptive_label_boundary_subdivides},
  {"adaptive", "depth_discontinuity_subdivides", adaptive_depth_discontinuity_subdivides},
  {"adaptive", "normal_discontinuity_subdivides", adaptive_normal_discontinuity_subdivides},
  {"adaptive", "tile_thread_determinism", adaptive_tile_thread_determinism},

  {"reconstruct", "voting_tie_and_coverage", reconstruct_voting_tie_and_coverage},
  {"reconstruct", "filter_weight_crosscheck", reconstruct_filter_weight_crosscheck},
  {"reconstruct", "no_label_averaging", reconstruct_no_label_averaging},
  {"reconstruct", "beauty_preserved_fixed", reconstruct_beauty_preserved_fixed},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
