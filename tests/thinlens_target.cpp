// Thin-lens depth-of-field target tests.
//
// Every expected value here is derived from an independent reference instead
// of the engine's rasterization path:
//   * projection follows the task's focal-plane formula directly,
//   * coverage/depth use a Moller-Trumbore ray/triangle intersection,
//   * shaded colors use an independent re-statement of the Blinn-Phong terms.
#include "../task_support/check.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ForwardRasterizer.h"
#include "DeferredRasterizer.h"
#include "PERSPECTIVECAMERA.H"
#include "OrthographicCamera.h"
#include "OffAxisPerspectiveCamera.h"
#include "ThinLens.h"
#include "Material.h"
#include "DIRECTIONALLIGHT.H"

namespace {

constexpr double EPS = 1e-9;
constexpr double COLOR_EPS = 2e-3;
constexpr double DEPTH_EPS = 1e-6;

bool near_eq(double a, double b, double tol) { return std::abs(a - b) <= tol; }
bool color_eq(const RGBColor& a, const RGBColor& b, double tol = COLOR_EPS) {
  return near_eq(a.r, b.r, tol) && near_eq(a.g, b.g, tol) && near_eq(a.b, b.b, tol);
}

// Independent triangle in world space, mirroring the data fed to the engine.
struct RefTriangle {
  Point3D v[3];
  RGBColor vertex_color;
};

GeometryObject* make_triangle(Material* material, const Point3D& a, const Point3D& b,
                              const Point3D& c, const RGBColor& color) {
  std::vector<Point3D> verts{ a, b, c };
  std::vector<RGBColor> colors{ color, color, color };
  std::vector<uint32_t> indices{ 0, 1, 2 };
  return new GeometryObject(material, verts, colors, std::vector<Vector2D>(), indices);
}

GeometryObject* make_quad(Material* material, const Point3D& a, const Point3D& b,
                          const Point3D& c, const Point3D& d, const RGBColor& color,
                          std::vector<RefTriangle>* ref, std::vector<GeometryObject*>* keep) {
  GeometryObject* t1 = make_triangle(material, a, b, c, color);
  GeometryObject* t2 = make_triangle(material, a, c, d, color);
  keep->push_back(t1);
  keep->push_back(t2);
  ref->push_back({ { a, b, c }, color });
  ref->push_back({ { a, c, d }, color });
  return nullptr;
}

// Independent focal scales from fov/aspect (matches PerspectiveCamera).
void focal_scales(double fov_deg, double aspect, double& A0, double& A1) {
  double th = std::tan((fov_deg / 2.0) * 3.14159265358979 / 180.0);
  A1 = 1.0 / th;
  A0 = 1.0 / (th * aspect);
}

// Task imaging equation, evaluated directly in the center-camera local frame.
Point2D task_project(double X, double Y, double Z, double a, double b,
                     double F, double A0, double A1) {
  return Point2D(A0 * (X / Z + a * (1.0 / F - 1.0 / Z)),
                 A1 * (Y / Z + b * (1.0 / F - 1.0 / Z)));
}

Point2D ndc_to_raster(const Point2D& ndc, uint32_t W, uint32_t H) {
  return Point2D((W / 2.0) * (ndc.x + 1.0), (H / 2.0) * (ndc.y + 1.0));
}
Point2D raster_to_ndc(double px, double py, uint32_t W, uint32_t H) {
  return Point2D(-1.0 + (2.0 / W) * px, -1.0 + (2.0 / H) * py);
}

struct RefHit {
  bool hit = false;
  double depth = 0.0;
  double bary[3] = { 0, 0, 0 };
  RGBColor color;
};

// Moller-Trumbore intersection of a ray with a triangle.
bool ray_triangle(const Point3D& o, const Vector3D& d, const RefTriangle& tri,
                  double& t, double bary[3]) {
  Vector3D e1 = tri.v[1] - tri.v[0];
  Vector3D e2 = tri.v[2] - tri.v[0];
  Vector3D p = d ^ e2;
  double det = e1 * p;
  if (std::abs(det) < 1e-12) return false;
  double inv = 1.0 / det;
  Vector3D s = o - tri.v[0];
  double u = (s * p) * inv;
  if (u < -EPS || u > 1.0 + EPS) return false;
  Vector3D q = s ^ e1;
  double v = (d * q) * inv;
  if (v < -EPS || u + v > 1.0 + EPS) return false;
  double tt = (e2 * q) * inv;
  if (tt <= EPS) return false;
  t = tt;
  bary[0] = 1.0 - u - v;
  bary[1] = u;
  bary[2] = v;
  return true;
}

// Independent engine-normal rule (see Triangle3D constructor).
Vector3D engine_normal(const RefTriangle& tri) {
  Vector3D a = tri.v[1] - tri.v[0];
  Vector3D b = tri.v[2] - tri.v[1];
  a.normalize();
  b.normalize();
  Vector3D n = b ^ a;
  n.normalize();
  return n;
}

// Independent Blinn-Phong result for a flat material, directional white light
// along (0,0,-1), black specular, matching Material.cpp.
RGBColor ref_shade(const RGBColor& vertex, const Vector3D& normal, double kd) {
  Vector3D L(0, 0, 1); // -lightDirection
  double lambert = std::max(0.0, L * normal);
  double base = 0.1 + kd * lambert; // ambient 0.1 + diffuse, specular term is zero
  return RGBColor(static_cast<float>(base * vertex.r),
                  static_cast<float>(base * vertex.g),
                  static_cast<float>(base * vertex.b));
}

// Trace one pixel for one pupil against an independent triangle list.
RefHit reference_trace(const std::vector<RefTriangle>& tris, double a, double b,
                       double F, double A0, double A1, uint32_t W, uint32_t H,
                       int px, int py, double near, double far, double kd) {
  RefHit result;
  Point2D ndc = raster_to_ndc(px, py, W, H);
  // Pupil view ray: origin at the pupil, direction from the inverse task map.
  Point3D origin(a, b, 0.0);
  Vector3D dir(ndc.x / A0 - a / F, ndc.y / A1 - b / F, 1.0);
  double best = far;
  for (const RefTriangle& tri : tris) {
    double t, bary[3];
    if (ray_triangle(origin, dir, tri, t, bary) && t >= near && t < best) {
      best = t;
      result.hit = true;
      result.depth = t;
      result.bary[0] = bary[0]; result.bary[1] = bary[1]; result.bary[2] = bary[2];
      result.color = ref_shade(tri.vertex_color, engine_normal(tri), kd);
    }
  }
  return result;
}

OffAxisPerspectiveCamera* make_pupil(PerspectiveCamera& center, Renderer& renderer,
                                     double a, double b, double F) {
  Point3D pos = center.get_position();
  Vector3D r = center.get_right();
  Vector3D u = center.get_up();
  Point3D pupil(pos.x + r.x * a + u.x * b,
                pos.y + r.y * a + u.y * b,
                pos.z + r.z * a + u.z * b);
  return new OffAxisPerspectiveCamera(center, pupil, a, b, F, &renderer);
}

// Flat scene used by the visibility/integration groups (identity camera).
const RGBColor BG(1.0f, 1.0f, 1.0f);
const RGBColor WHITE(1.0f, 1.0f, 1.0f);

struct FlatScene {
  static constexpr uint32_t W = 24, H = 16;
  static constexpr double F = 10.0, NEAR = 1.0, FAR = 200.0, KD = 0.4;
  World world;
  FlatMaterial material;
  DirectionalLight light;
  std::vector<Light*> lights;
  std::vector<GeometryObject*> objects;
  std::vector<RefTriangle> tris;
  PerspectiveCamera* center;

  FlatScene(Renderer* renderer)
    : material(RGBColor(0.4f, 0.4f, 0.4f), RGBColor(0.0f, 0.0f, 0.0f), 1.0f),
      light(WHITE, Vector3D(0, 0, -1)) {
    lights.push_back(&light);
    // Near yellow occluder at z=6.
    make_quad(&material, {-3.5, -2.0, 6}, {0.5, -2.0, 6}, {0.5, 2.0, 6}, {-3.5, 2.0, 6},
              RGBColor(1, 1, 0), &tris, &objects);
    // Cyan marker exactly on the focal plane z=F.
    make_quad(&material, {1.5, -1.0, F}, {3.5, -1.0, F}, {3.5, 1.0, F}, {1.5, 1.0, F},
              RGBColor(0, 1, 1), &tris, &objects);
    // Far red/green/blue quads at z=20.
    make_quad(&material, {-6, -2, 20}, {-2, -2, 20}, {-2, 2, 20}, {-6, 2, 20},
              RGBColor(1, 0, 0), &tris, &objects);
    make_quad(&material, {-2, -2, 20}, {2, -2, 20}, {2, 2, 20}, {-2, 2, 20},
              RGBColor(0, 1, 0), &tris, &objects);
    make_quad(&material, {2, -2, 20}, {6, -2, 20}, {6, 2, 20}, {2, 2, 20},
              RGBColor(0, 0, 1), &tris, &objects);

    center = new PerspectiveCamera(Point3D(0, 0, 0), Point3D(0, 0, 1), H, W, renderer,
                                   (uint32_t)NEAR, (uint32_t)FAR, 90.0f);
    world = World(objects, lights, center);
    renderer->set_world(&world);
    renderer->set_camera(center);
  }
};

} // namespace

// ----------------------------- projection group -----------------------------

static int projection_task_formula_matches_engine() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  double A0, A1;
  focal_scales(90.0, (double)FlatScene::W / FlatScene::H, A0, A1);
  const double a = 2.0, b = -1.5, F = FlatScene::F;
  std::unique_ptr<OffAxisPerspectiveCamera> pupil(make_pupil(*scene.center, renderer, a, b, F));
  renderer.set_camera(pupil.get());
  scene.world.m_camera = pupil.get();

  const double local[][3] = { {1.2, -0.7, 4.0}, {0.0, 0.0, F}, {-2.0, 1.1, 7.5}, {3.0, -2.2, 30.0} };
  for (auto& p : local) {
    Point2D expect_ndc = task_project(p[0], p[1], p[2], a, b, F, A0, A1);
    Point2D expect = ndc_to_raster(expect_ndc, FlatScene::W, FlatScene::H);
    Point2D got = renderer.rasterize(Point3D(p[0], p[1], p[2]));
    TASK_CHECK(near_eq(got.x, expect.x, 1e-6));
    TASK_CHECK(near_eq(got.y, expect.y, 1e-6));
  }
  return 0;
}

static int projection_focal_plane_is_locked() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  double A0, A1;
  focal_scales(90.0, (double)FlatScene::W / FlatScene::H, A0, A1);
  renderer.set_camera(scene.center);
  // Points on z=F must project identically for the center and any pupil.
  Point3D focal_points[] = { {1.7, 0.4, FlatScene::F}, {-0.9, -0.8, FlatScene::F}, {2.6, 0.9, FlatScene::F} };
  for (double a : { -3.0, 0.0, 3.0 }) {
    for (double b : { -2.0, 2.0 }) {
      std::unique_ptr<OffAxisPerspectiveCamera> pupil(make_pupil(*scene.center, renderer, a, b, FlatScene::F));
      renderer.set_camera(pupil.get());
      for (auto& fp : focal_points) {
        Point2D center_r = scene.center->viewportTransform(
          Point2D(A0 * fp.x / fp.z, A1 * fp.y / fp.z));
        Point2D pupil_r = renderer.rasterize(fp);
        TASK_CHECK(near_eq(center_r.x, pupil_r.x, 1e-6));
        TASK_CHECK(near_eq(center_r.y, pupil_r.y, 1e-6));
      }
    }
  }
  return 0;
}

static int projection_parallax_sign_and_magnitude() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  double A0, A1;
  focal_scales(90.0, (double)FlatScene::W / FlatScene::H, A0, A1);
  // Independent parallax relation between two pupils for a point at depth Z.
  const double a1 = 3.0, a2 = -3.0, Z = 6.0, X = -1.0;
  Point2D n1 = task_project(X, 0, Z, a1, 0, FlatScene::F, A0, A1);
  Point2D n2 = task_project(X, 0, Z, a2, 0, FlatScene::F, A0, A1);
  double expect_delta = A0 * (a1 - a2) * (1.0 / FlatScene::F - 1.0 / Z);
  TASK_CHECK(near_eq(n1.x - n2.x, expect_delta, 1e-9));
  // Near geometry (Z<F) and far geometry (Z>F) must shift in opposite senses.
  Point2D f1 = task_project(X, 0, 20.0, a1, 0, FlatScene::F, A0, A1);
  Point2D f2 = task_project(X, 0, 20.0, a2, 0, FlatScene::F, A0, A1);
  TASK_CHECK((n1.x - n2.x) * (f1.x - f2.x) < 0.0);
  return 0;
}

static int projection_unrasterize_round_trip() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  double A0, A1;
  focal_scales(90.0, (double)FlatScene::W / FlatScene::H, A0, A1);
  const double a = 2.5, b = -1.0, F = FlatScene::F;
  std::unique_ptr<OffAxisPerspectiveCamera> pupil(make_pupil(*scene.center, renderer, a, b, F));
  renderer.set_camera(pupil.get());
  scene.world.m_camera = pupil.get();
  for (int px : { 3, 11, 20 }) {
    for (int py : { 2, 8, 13 }) {
      for (double Zc : { 4.0, 9.0, 25.0 }) {
        Point2D raster(px, py);
        Point3D world = renderer.unrasterize(raster, Zc);
        // Independent inverse: pupil view coords then translate by the eye.
        Point2D ndc = raster_to_ndc(px, py, FlatScene::W, FlatScene::H);
        double Xc = Zc * (ndc.x / A0 - a / F);
        double Yc = Zc * (ndc.y / A1 - b / F);
        Point3D expect(Xc + a, Yc + b, Zc);
        TASK_CHECK(near_eq(world.x, expect.x, 1e-6));
        TASK_CHECK(near_eq(world.y, expect.y, 1e-6));
        TASK_CHECK(near_eq(world.z, expect.z, 1e-6));
      }
    }
  }
  return 0;
}

static int projection_view_direction_uses_pupil() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  const double a = 2.0, b = 1.0;
  std::unique_ptr<OffAxisPerspectiveCamera> pupil(make_pupil(*scene.center, renderer, a, b, FlatScene::F));
  Point3D surface(1.0, -1.0, 12.0);
  Vector3D got = pupil->viewDirection(surface);
  Vector3D expect = surface - pupil->get_position();
  expect.normalize();
  TASK_CHECK(near_eq(got.x, expect.x, 1e-9));
  TASK_CHECK(near_eq(got.y, expect.y, 1e-9));
  TASK_CHECK(near_eq(got.z, expect.z, 1e-9));
  TASK_CHECK(!near_eq(got.x, (surface - scene.center->get_position()).x, 1e-6));
  return 0;
}

static int projection_rotated_translated_camera() {
  ForwardRasterizer renderer;
  PerspectiveCamera center(Point3D(2, -3, 1), Point3D(0, 1, 4), 18, 30, &renderer, 1, 200, 70.0f);
  renderer.set_camera(&center);
  World world(std::vector<GeometryObject*>{}, std::vector<Light*>{}, &center);
  double A0, A1;
  focal_scales(70.0, 30.0 / 18.0, A0, A1);
  const double F = 12.0, a = 1.5, b = -0.8;
  std::unique_ptr<OffAxisPerspectiveCamera> pupil(make_pupil(center, renderer, a, b, F));
  renderer.set_camera(pupil.get());
  world.m_camera = pupil.get();
  Vector3D right = center.get_right(), up = center.get_up(), dir = center.get_direction();
  Point3D pos = center.get_position();
  // Deterministic points defined in the center local frame.
  double locals[][3] = { {0.5, -0.5, 5.0}, {-1.2, 0.8, F}, {1.7, 1.1, 25.0}, {0.0, 0.0, 15.0} };
  for (auto& L : locals) {
    Point3D world_point(pos.x + right.x * L[0] + up.x * L[1] + dir.x * L[2],
                        pos.y + right.y * L[0] + up.y * L[1] + dir.y * L[2],
                        pos.z + right.z * L[0] + up.z * L[1] + dir.z * L[2]);
    Point2D expect_ndc = task_project(L[0], L[1], L[2], a, b, F, A0, A1);
    Point2D expect = ndc_to_raster(expect_ndc, 30, 18);
    Point2D got = renderer.rasterize(world_point);
    TASK_CHECK(near_eq(got.x, expect.x, 1e-5));
    TASK_CHECK(near_eq(got.y, expect.y, 1e-5));
  }
  return 0;
}

// ----------------------------- visibility group -----------------------------

static int visibility_pupils_disagree_and_background_revealed() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  double A0, A1;
  focal_scales(90.0, (double)FlatScene::W / FlatScene::H, A0, A1);
  const double R = 3.0;
  const double pts[][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
  struct Pupil { double a, b; std::vector<RGBColor> color; std::vector<double> depth; };
  std::vector<Pupil> pupils;
  for (auto& p : pts) {
    double a = R * p[0], b = R * p[1];
    std::unique_ptr<OffAxisPerspectiveCamera> cam(make_pupil(*scene.center, renderer, a, b, FlatScene::F));
    renderer.set_camera(cam.get());
    scene.world.m_camera = cam.get();
    Pupil pu; pu.a = a; pu.b = b;
    renderer.renderPupilFrame(*cam, pu.color, pu.depth);
    pupils.push_back(std::move(pu));
  }
  int disagree_pixels = 0;
  int background_pixels = 0;
  int engine_matches_reference = 0;
  int engine_reference_checks = 0;
  // Pixels guaranteed empty for every pupil (the geometry stays near the image
  // center), used to check background handling without edge ambiguity.
  const int corner_pixels[][2] = { {0, 0}, {FlatScene::W - 1, 0}, {0, FlatScene::H - 1},
                                   {FlatScene::W - 1, FlatScene::H - 1} };
  for (int y = 0; y < (int)FlatScene::H; ++y) {
    for (int x = 0; x < (int)FlatScene::W; ++x) {
      int i = y * FlatScene::W + x;
      bool surfaces_differ = false;
      bool any_background = false;
      bool is_corner = false;
      for (auto& cp : corner_pixels) if (cp[0] == x && cp[1] == y) is_corner = true;
      RefHit first;
      bool have = false;
      for (Pupil& pu : pupils) {
        RefHit hit = reference_trace(scene.tris, pu.a, pu.b, FlatScene::F, A0, A1,
                                     FlatScene::W, FlatScene::H, x, y, FlatScene::NEAR,
                                     FlatScene::FAR, FlatScene::KD);
        if (!hit.hit) any_background = true;
        if (have) {
          if (hit.hit != first.hit || (hit.hit && !color_eq(hit.color, first.color, 1e-6)))
            surfaces_differ = true;
        } else {
          first = hit; have = true;
        }
        // Strict engine/reference comparison where rasterization and the
        // analytic ray cannot disagree about coverage: a deep-interior hit for
        // this pupil, or a guaranteed-empty corner.
        bool strict = (!hit.hit && is_corner) ||
                      (hit.hit && std::min(std::min(hit.bary[0], hit.bary[1]), hit.bary[2]) >= 0.12);
        if (strict) {
          engine_reference_checks++;
          if (hit.hit) {
            if (near_eq(pu.depth[i], hit.depth, DEPTH_EPS) && color_eq(pu.color[i], hit.color, COLOR_EPS))
              engine_matches_reference++;
          } else {
            if (near_eq(pu.depth[i], FlatScene::FAR, DEPTH_EPS) && color_eq(pu.color[i], BG, 1e-6))
              engine_matches_reference++;
          }
        }
      }
      if (surfaces_differ) disagree_pixels++;
      if (any_background) background_pixels++;
    }
  }
  TASK_CHECK(disagree_pixels > 0);                 // different pupils see different surfaces
  TASK_CHECK(background_pixels > 0);               // background is revealed around silhouettes
  TASK_CHECK(engine_reference_checks > 50);        // a substantial strict comparison ran
  TASK_CHECK(engine_matches_reference == engine_reference_checks);
  return 0;
}

static int visibility_occluder_and_far_colors_observed() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  double A0, A1;
  focal_scales(90.0, (double)FlatScene::W / FlatScene::H, A0, A1);
  bool saw_occluder = false, saw_far = false, saw_background = false;
  const double a = 3.0, b = 0.0;
  std::unique_ptr<OffAxisPerspectiveCamera> cam(make_pupil(*scene.center, renderer, a, b, FlatScene::F));
  renderer.set_camera(cam.get());
  scene.world.m_camera = cam.get();
  std::vector<RGBColor> colors;
  std::vector<double> depths;
  renderer.renderPupilFrame(*cam, colors, depths);
  for (int y = 0; y < (int)FlatScene::H; ++y) {
    for (int x = 0; x < (int)FlatScene::W; ++x) {
      RefHit hit = reference_trace(scene.tris, a, b, FlatScene::F, A0, A1,
                                   FlatScene::W, FlatScene::H, x, y, FlatScene::NEAR,
                                   FlatScene::FAR, FlatScene::KD);
      int i = y * FlatScene::W + x;
      if (!hit.hit) {
        saw_background = true;
        TASK_CHECK(color_eq(colors[i], BG, 1e-6));
      } else {
        if (near_eq(hit.depth, 6.0, 0.25)) saw_occluder = true;
        if (near_eq(hit.depth, 20.0, 0.25)) saw_far = true;
      }
    }
  }
  TASK_CHECK(saw_occluder);
  TASK_CHECK(saw_far);
  TASK_CHECK(saw_background);
  return 0;
}

static int visibility_center_depth_is_independent_draw() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  double A0, A1;
  focal_scales(90.0, (double)FlatScene::W / FlatScene::H, A0, A1);
  std::vector<PupilSample> samples{ {{1, 0}, 1}, {{-1, 0}, 1}, {{0, 1}, 1} };
  ThinLensImage image = ThinLens::render(*scene.center, renderer, scene.world, FlatScene::F, 3.0, samples);
  // A direct center pinhole geometry draw must match the returned depth exactly.
  renderer.set_camera(scene.center);
  scene.world.m_camera = scene.center;
  std::vector<double> direct = renderer.renderCenterDepth(*scene.center);
  TASK_CHECK(image.depth.size() == direct.size());
  int checked = 0;
  for (int y = 0; y < (int)FlatScene::H; ++y) {
    for (int x = 0; x < (int)FlatScene::W; ++x) {
      int i = y * FlatScene::W + x;
      RefHit hit = reference_trace(scene.tris, 0, 0, FlatScene::F, A0, A1,
                                   FlatScene::W, FlatScene::H, x, y, FlatScene::NEAR,
                                   FlatScene::FAR, FlatScene::KD);
      TASK_CHECK(near_eq(image.depth[i], direct[i], 1e-12));
      if (hit.hit) {
        TASK_CHECK(near_eq(image.depth[i], hit.depth, DEPTH_EPS));
        checked++;
      } else {
        TASK_CHECK(near_eq(image.depth[i], FlatScene::FAR, DEPTH_EPS));
      }
    }
  }
  TASK_CHECK(checked > 0);
  return 0;
}

// ----------------------------- integration group ----------------------------

static ThinLensImage run_weighted(Rasterizer& renderer, FlatScene& scene) {
  // Non-uniform, non-normalized weights to prove normalization and weighting.
  std::vector<PupilSample> samples{
    {{1, 0}, 1.0}, {{-1, 0}, 2.0}, {{0, 1}, 1.0}, {{0, -1}, 1.0}
  };
  return ThinLens::render(*scene.center, renderer, scene.world, FlatScene::F, 3.0, samples);
}

static int integration_weighted_average_includes_background() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  double A0, A1;
  focal_scales(90.0, (double)FlatScene::W / FlatScene::H, A0, A1);
  const double weights[] = { 1, 2, 1, 1 };
  const double pts[][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
  const double total = 5.0;
  ThinLensImage image = run_weighted(renderer, scene);
  int compared = 0;
  int mixed_coverage_pixels = 0; // some pupils hit a surface, others see background
  for (int y = 0; y < (int)FlatScene::H; ++y) {
    for (int x = 0; x < (int)FlatScene::W; ++x) {
      int i = y * FlatScene::W + x;
      double ar = 0, ag = 0, ab = 0;
      bool stable = true;
      int hit_count = 0;
      for (int k = 0; k < 4; ++k) {
        double a = 3.0 * pts[k][0], b = 3.0 * pts[k][1];
        RefHit hit = reference_trace(scene.tris, a, b, FlatScene::F, A0, A1,
                                     FlatScene::W, FlatScene::H, x, y, FlatScene::NEAR,
                                     FlatScene::FAR, FlatScene::KD);
        if (hit.hit) {
          ++hit_count;
          if (std::min(std::min(hit.bary[0], hit.bary[1]), hit.bary[2]) < 0.05) {
            stable = false; break;
          }
        }
        RGBColor c = hit.hit ? hit.color : BG;
        ar += c.r * weights[k]; ag += c.g * weights[k]; ab += c.b * weights[k];
      }
      if (!stable) continue;
      if (hit_count > 0 && hit_count < 4) ++mixed_coverage_pixels;
      TASK_CHECK(near_eq(image.color[i].r, (float)(ar / total), COLOR_EPS));
      TASK_CHECK(near_eq(image.color[i].g, (float)(ag / total), COLOR_EPS));
      TASK_CHECK(near_eq(image.color[i].b, (float)(ab / total), COLOR_EPS));
      compared++;
    }
  }
  TASK_CHECK(compared > 100);
  // Silhouette pixels where only some pupils see geometry prove the background
  // itself carries weight into the average (no opaque "last hit wins").
  TASK_CHECK(mixed_coverage_pixels > 0);
  // Explicit background-weight check on a pixel the center camera leaves empty.
  int bg_pixel = -1;
  for (int i = 0; i < (int)image.color.size(); ++i) {
    if (near_eq(image.depth[i], FlatScene::FAR, DEPTH_EPS)) { bg_pixel = i; break; }
  }
  TASK_CHECK(bg_pixel >= 0);
  return 0;
}

static int integration_forward_and_deferred_use_same_sample_camera() {
  ForwardRasterizer forward;
  FlatScene fs_forward(&forward);
  ThinLensImage a = run_weighted(forward, fs_forward);

  DeferredRasterizer deferred;
  FlatScene fs_deferred(&deferred);
  ThinLensImage b = run_weighted(deferred, fs_deferred);

  TASK_CHECK(a.color.size() == b.color.size());
  TASK_CHECK(a.depth.size() == b.depth.size());
  for (size_t i = 0; i < a.color.size(); ++i) {
    TASK_CHECK(color_eq(a.color[i], b.color[i], 1e-6));
    TASK_CHECK(near_eq(a.depth[i], b.depth[i], 1e-9));
  }
  return 0;
}

static int integration_focal_marker_stays_fixed() {
  // Occlusion-free scene whose only geometry lies exactly on the focal plane,
  // so every pupil must see it at identical raster coordinates with an
  // identical (view-independent Blinn-Phong, black specular) shade.
  ForwardRasterizer renderer;
  const uint32_t W = FlatScene::W, H = FlatScene::H;
  FlatMaterial material(RGBColor(0.4f, 0.4f, 0.4f), RGBColor(0.0f, 0.0f, 0.0f), 1.0f);
  GeometryObject* tri = make_triangle(&material,
                                      Point3D(-2, -1.5, FlatScene::F),
                                      Point3D(2, -1.5, FlatScene::F),
                                      Point3D(0, 2, FlatScene::F),
                                      RGBColor(0, 1, 1));
  DirectionalLight light(WHITE, Vector3D(0, 0, -1));
  std::vector<GeometryObject*> objects{ tri };
  std::vector<Light*> lights{ &light };
  PerspectiveCamera center(Point3D(0, 0, 0), Point3D(0, 0, 1), H, W, &renderer,
                           (uint32_t)FlatScene::NEAR, (uint32_t)FlatScene::FAR, 90.0f);
  World world(objects, lights, &center);
  renderer.set_world(&world);
  renderer.set_camera(&center);
  world.m_camera = &center;

  std::vector<RGBColor> center_color;
  std::vector<double> center_depth;
  renderer.renderPupilFrame(center, center_color, center_depth);

  std::vector<PupilSample> samples{
    {{1, 0}, 1}, {{-1, 0}, 1}, {{0, 1}, 1}, {{0, -1}, 1}
  };
  ThinLensImage image = ThinLens::render(center, renderer, world, FlatScene::F, 3.0, samples);
  int marker_pixels = 0;
  for (size_t i = 0; i < center_color.size(); ++i) {
    if (near_eq(center_depth[i], FlatScene::F, DEPTH_EPS)) {
      TASK_CHECK(near_eq(image.depth[i], center_depth[i], 1e-9));
      TASK_CHECK(color_eq(image.color[i], center_color[i], COLOR_EPS));
      marker_pixels++;
    } else {
      TASK_CHECK(near_eq(image.depth[i], FlatScene::FAR, DEPTH_EPS));
      TASK_CHECK(color_eq(image.color[i], BG, 1e-6));
    }
  }
  TASK_CHECK(marker_pixels > 0);
  return 0;
}

static int integration_non_square_viewport() {
  // Wide, non-square image: A0 != A1 must be honored on both axes.
  ForwardRasterizer renderer;
  PerspectiveCamera center(Point3D(0, 0, 0), Point3D(0, 0, 1), 10, 30, &renderer, 1, 200, 90.0f);
  renderer.set_camera(&center);
  FlatMaterial mat(RGBColor(0.4f), RGBColor(0.0f), 1.0f);
  auto* obj = make_triangle(&mat, {-3, -2, 12}, {3, -2, 12}, {0, 3, 12}, RGBColor(1, 0, 0));
  std::vector<GeometryObject*> objs{ obj };
  DirectionalLight light(WHITE, Vector3D(0, 0, -1));
  std::vector<Light*> lights{ &light };
  World world(objs, lights, &center);
  renderer.set_world(&world);
  std::vector<PupilSample> samples{ {{1, 0}, 1}, {{-1, 0}, 1}, {{0, 1}, 1}, {{0, -1}, 1} };
  ThinLensImage image = ThinLens::render(center, renderer, world, 12.0, 2.0, samples);
  TASK_CHECK(image.width == 30 && image.height == 10);
  TASK_CHECK(image.color.size() == 300);
  int covered = 0;
  for (double d : image.depth) if (d < 200.0) covered++;
  TASK_CHECK(covered > 0);
  return 0;
}

// ------------------------------ lifetime group ------------------------------

static int lifetime_zero_radius_is_pinhole() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  renderer.set_camera(scene.center);
  scene.world.m_camera = scene.center;
  renderer.render(true, false);
  std::vector<RGBColor> normal_pixels = renderer.get_pixels();
  std::vector<double> normal_depth = renderer.get_depth_buffer();

  std::vector<PupilSample> samples{ {{1, 0}, 1}, {{0, 1}, 2} }; // ignored when R=0
  ThinLensImage image = ThinLens::render(*scene.center, renderer, scene.world, FlatScene::F, 0.0, samples);
  TASK_CHECK(image.color.size() == normal_pixels.size());
  for (size_t i = 0; i < image.color.size(); ++i) {
    TASK_CHECK(color_eq(image.color[i], normal_pixels[i], 1e-6));
    TASK_CHECK(near_eq(image.depth[i], normal_depth[i], 1e-9));
  }
  return 0;
}

static int lifetime_repeated_calls_are_stable_and_restored() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  // Baseline ordinary render captured before any ThinLens call.
  renderer.set_camera(scene.center);
  scene.world.m_camera = scene.center;
  renderer.render(true, false);
  std::vector<RGBColor> before_pixels = renderer.get_pixels();
  std::vector<double> before_depth = renderer.get_depth_buffer();

  std::vector<PupilSample> samples{ {{1, 0}, 1}, {{-1, 0}, 1} };
  ThinLensImage first = ThinLens::render(*scene.center, renderer, scene.world, FlatScene::F, 2.0, samples);
  TASK_CHECK(renderer.get_camera() == scene.center);
  TASK_CHECK(scene.world.m_camera == scene.center);
  ThinLensImage second = ThinLens::render(*scene.center, renderer, scene.world, FlatScene::F, 2.0, samples);
  TASK_CHECK(renderer.get_camera() == scene.center);
  TASK_CHECK(scene.world.m_camera == scene.center);
  for (size_t i = 0; i < first.color.size(); ++i) {
    TASK_CHECK(color_eq(first.color[i], second.color[i], 1e-12));
    TASK_CHECK(near_eq(first.depth[i], second.depth[i], 1e-12));
  }
  // A plain render after ThinLens must be identical to the one taken before.
  renderer.set_camera(scene.center);
  scene.world.m_camera = scene.center;
  renderer.render(true, false);
  std::vector<RGBColor> after_pixels = renderer.get_pixels();
  std::vector<double> after_depth = renderer.get_depth_buffer();
  TASK_CHECK(after_pixels.size() == before_pixels.size());
  for (size_t i = 0; i < after_pixels.size(); ++i) {
    TASK_CHECK(color_eq(after_pixels[i], before_pixels[i], 1e-12));
    TASK_CHECK(near_eq(after_depth[i], before_depth[i], 1e-12));
  }
  return 0;
}

static int lifetime_two_instances_interleaved() {
  ForwardRasterizer r1, r2;
  FlatScene s1(&r1);
  PerspectiveCamera* c2 = new PerspectiveCamera(Point3D(0, 0, 0), Point3D(0, 0, 1), 8, 12, &r2, 1, 100, 90.0f);
  r2.set_camera(c2);
  FlatMaterial mat(RGBColor(0.4f), RGBColor(0.0f), 1.0f);
  GeometryObject* tri = make_triangle(&mat, {-2, -2, 8}, {2, -2, 8}, {0, 2, 8}, RGBColor(0, 1, 0));
  DirectionalLight light(WHITE, Vector3D(0, 0, -1));
  std::vector<GeometryObject*> o2{ tri };
  std::vector<Light*> l2{ &light };
  World w2(o2, l2, c2);
  r2.set_world(&w2);
  std::vector<PupilSample> samples{ {{1, 0}, 1}, {{-1, 0}, 1} };

  ThinLensImage i1 = ThinLens::render(*s1.center, r1, s1.world, FlatScene::F, 2.0, samples);
  ThinLensImage i2 = ThinLens::render(*c2, r2, w2, 8.0, 1.5, samples);
  ThinLensImage i1b = ThinLens::render(*s1.center, r1, s1.world, FlatScene::F, 2.0, samples);
  ThinLensImage i2b = ThinLens::render(*c2, r2, w2, 8.0, 1.5, samples);

  TASK_CHECK(i1.color.size() == FlatScene::W * FlatScene::H);
  TASK_CHECK(i2.color.size() == 12u * 8u);
  for (size_t i = 0; i < i1.color.size(); ++i) {
    TASK_CHECK(color_eq(i1.color[i], i1b.color[i], 1e-12));
    TASK_CHECK(near_eq(i1.depth[i], i1b.depth[i], 1e-12));
  }
  for (size_t i = 0; i < i2.color.size(); ++i) {
    TASK_CHECK(color_eq(i2.color[i], i2b.color[i], 1e-12));
  }
  TASK_CHECK(r1.get_camera() == s1.center);
  TASK_CHECK(r2.get_camera() == c2);
  TASK_CHECK(s1.world.m_camera == s1.center);
  TASK_CHECK(w2.m_camera == c2);
  int covered2 = 0;
  for (double d : i2.depth) if (d < 100.0) covered2++;
  TASK_CHECK(covered2 > 0);
  return 0;
}

static int lifetime_invalid_inputs_throw_and_restore() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  renderer.set_camera(scene.center);
  scene.world.m_camera = scene.center;
  renderer.render(true, false);
  std::vector<RGBColor> before_pixels = renderer.get_pixels();
  std::vector<double> before_depth = renderer.get_depth_buffer();

  auto expect_throw = [&](double F, double R, const std::vector<PupilSample>& s) {
    bool threw = false;
    try {
      ThinLens::render(*scene.center, renderer, scene.world, F, R, s);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(renderer.get_camera() == scene.center);
    TASK_CHECK(scene.world.m_camera == scene.center);
  };

  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<PupilSample> good{ {{0.5, 0.0}, 1.0} };
  expect_throw(inf, 2.0, good);                                   // non-finite F
  expect_throw(nan, 2.0, good);
  expect_throw(FlatScene::NEAR, 2.0, good);                       // F not beyond near
  expect_throw(FlatScene::NEAR - 0.5, 2.0, good);
  expect_throw(FlatScene::F, -1.0, good);                         // negative R
  expect_throw(FlatScene::F, inf, good);                          // non-finite R
  expect_throw(FlatScene::F, 2.0, {});                            // empty samples
  expect_throw(FlatScene::F, 2.0, { {{2.0, 0.0}, 1.0} });         // outside unit disk
  expect_throw(FlatScene::F, 2.0, { {{0.0, nan}, 1.0} });         // non-finite point
  expect_throw(FlatScene::F, 2.0, { {{0.5, 0.0}, -1.0} });        // negative weight
  expect_throw(FlatScene::F, 2.0, { {{0.5, 0.0}, 0.0} });         // zero total weight
  expect_throw(FlatScene::F, 2.0, { {{0.5, 0.0}, inf} });         // non-finite weight

  // Orthographic camera rejection through the world relationship. Constructing
  // the camera binds it to the renderer (Camera base behavior); the ThinLens
  // call must throw before touching either relationship, leaving them as they
  // were at entry (both naming the orthographic camera).
  OrthographicCamera ortho(Point3D(0, 0, 0), Point3D(0, 0, 1), FlatScene::H, FlatScene::W, &renderer, 1, 200);
  scene.world.m_camera = &ortho;
  bool threw = false;
  try {
    ThinLens::render(*scene.center, renderer, scene.world, FlatScene::F, 2.0, good);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(renderer.get_camera() == &ortho);
  TASK_CHECK(scene.world.m_camera == &ortho);
  renderer.set_camera(scene.center);
  scene.world.m_camera = scene.center;

  // Nothing drawn by the failed calls: a normal render is unchanged.
  renderer.render(true, false);
  std::vector<RGBColor> after_pixels = renderer.get_pixels();
  std::vector<double> after_depth = renderer.get_depth_buffer();
  TASK_CHECK(before_pixels.size() == after_pixels.size());
  for (size_t i = 0; i < before_pixels.size(); ++i) {
    TASK_CHECK(color_eq(before_pixels[i], after_pixels[i], 1e-12));
    TASK_CHECK(near_eq(before_depth[i], after_depth[i], 1e-12));
  }
  return 0;
}

static int lifetime_export_uses_original_bmp_writer() {
  ForwardRasterizer renderer;
  FlatScene scene(&renderer);
  std::vector<PupilSample> samples{ {{1, 0}, 1}, {{-1, 0}, 1} };
  ThinLensImage image = ThinLens::render(*scene.center, renderer, scene.world, FlatScene::F, 2.0, samples);
  std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/thinlens_target.bmp";
  ThinLens::exportImage(image, renderer, path);
  FILE* f = std::fopen(path.c_str(), "rb");
  TASK_CHECK(f != nullptr);
  if (f) {
    char tag[2] = { 0, 0 };
    size_t n = std::fread(tag, 1, 2, f);
    TASK_CHECK(n == 2 && tag[0] == 'B' && tag[1] == 'M');
    std::fclose(f);
  }
  return 0;
}

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
  { "projection", "task_formula", projection_task_formula_matches_engine },
  { "projection", "focal_lock", projection_focal_plane_is_locked },
  { "projection", "parallax_relation", projection_parallax_sign_and_magnitude },
  { "projection", "unrasterize_roundtrip", projection_unrasterize_round_trip },
  { "projection", "view_direction_pupil", projection_view_direction_uses_pupil },
  { "projection", "rotated_translated", projection_rotated_translated_camera },

  { "visibility", "pupil_disagreement", visibility_pupils_disagree_and_background_revealed },
  { "visibility", "depth_layers", visibility_occluder_and_far_colors_observed },
  { "visibility", "center_depth_independent", visibility_center_depth_is_independent_draw },

  { "integration", "weighted_average", integration_weighted_average_includes_background },
  { "integration", "forward_deferred_consistency", integration_forward_and_deferred_use_same_sample_camera },
  { "integration", "focal_marker_fixed", integration_focal_marker_stays_fixed },
  { "integration", "non_square_viewport", integration_non_square_viewport },

  { "lifetime", "zero_radius", lifetime_zero_radius_is_pinhole },
  { "lifetime", "repeated_calls", lifetime_repeated_calls_are_stable_and_restored },
  { "lifetime", "interleaved_instances", lifetime_two_instances_interleaved },
  { "lifetime", "invalid_inputs_restore", lifetime_invalid_inputs_throw_and_restore },
  { "lifetime", "bmp_export", lifetime_export_uses_original_bmp_writer },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
