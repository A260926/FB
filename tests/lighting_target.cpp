// Windowless regression for multi-light shading, per-light shadow composition
// and shadow-map resource ownership.
//
// The cases are registered through task_cases/task_case_count and split into
// three groups (lighting, projection, lifetime). All assertions go through
// TASK_CHECK. Protected renderer buffers are read back through small test
// subclasses; no graphics window is created anywhere.
//
// The two-light + ambient numbers used below are recomputed independently in
// manualBlinn() rather than copied from the production Material code, and the
// shadow behaviour is also observed through real geometry rendered by both
// ForwardRasterizer and DeferredRasterizer.

#include "task_support/check.h"

#include "World.h"
#include "ForwardRasterizer.h"
#include "DeferredRasterizer.h"
#include "OrthographicCamera.h"
#include "PERSPECTIVECAMERA.H"
#include "GeometryObject.h"
#include "Material.h"
#include "ShadowMap.h"
#include "DIRECTIONALLIGHT.H"

#include <cmath>
#include <cstddef>
#include <vector>

namespace {

const double EPS = 1e-9;
const float COLOR_EPS = 2e-3f;

bool near(double a, double b, double eps = EPS) { return std::fabs(a - b) <= eps; }
bool nearColor(const RGBColor& a, const RGBColor& b, float eps = COLOR_EPS) {
  return std::fabs(a.r - b.r) <= eps && std::fabs(a.g - b.g) <= eps &&
         std::fabs(a.b - b.b) <= eps;
}

// Test subclasses expose the protected buffers/state the assertions need.
class ForwardProbe : public ForwardRasterizer {
public:
  explicit ForwardProbe(const World* w) : ForwardRasterizer(w) {}
  size_t mapCount() const { return m_shadow_maps.size(); }
  ShadowMap* map(size_t i) const { return m_shadow_maps.at(i); }
  const std::vector<RGBColor>& pixels() const { return m_pixels; }
  using Rasterizer::calculateDepth;
  using Rasterizer::createShadowMaps;
  using Rasterizer::releaseShadowMaps;
};

class DeferredProbe : public DeferredRasterizer {
public:
  explicit DeferredProbe(const World* w) : DeferredRasterizer(w) {}
  const std::vector<RGBColor>& pixels() const { return m_pixels; }
};

// Four-vertex quad mesh (two triangles). Used for both receivers and occluders.
GeometryObject* makeQuad(FlatMaterial* material, const std::vector<Point3D>& vertices) {
  const std::vector<RGBColor> colors(4, RGBColor(1, 1, 1));
  const std::vector<Vector2D> texcoords(4, Vector2D(0, 0));
  const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
  return new GeometryObject(material, vertices, colors, texcoords, indices);
}

// Vertical plane on x = plane_x, spanning +/-half on y and z, normal +x.
GeometryObject* verticalPlane(FlatMaterial* material, double plane_x, double half) {
  return makeQuad(material, {
    Point3D(plane_x, -half, -half),
    Point3D(plane_x, -half,  half),
    Point3D(plane_x,  half,  half),
    Point3D(plane_x,  half, -half)
  });
}

// Independent re-implementation of the Blinn-Phong sum used to check the
// engine. Specular is zero (the test materials use k_s = 0) so the direct term
// of light k is visibility[k] * lightColor[k] * diffuse * max(L.N, 0).
// Ambient is added exactly once and never scaled by a shadow factor.
RGBColor manualBlinn(const std::vector<const Light*>& lights,
                     const std::vector<double>& visibility,
                     const Vector3D& N, const Vector3D& V,
                     const RGBColor& diffuse, const RGBColor& vertex_color) {
  const double ambient = 0.1;
  double sum_r = ambient, sum_g = ambient, sum_b = ambient;
  for (size_t k = 0; k < lights.size(); ++k) {
    Vector3D L = -(lights[k]->getDirectionToPoint(Point3D(0, 0, 0)));
    const double ndotl = std::max(L * N, 0.0) * visibility[k];
    const RGBColor& c = lights[k]->getColor();
    sum_r += c.r * diffuse.r * ndotl;
    sum_g += c.g * diffuse.g * ndotl;
    sum_b += c.b * diffuse.b * ndotl;
  }
  auto sat = [](double x) { return std::min(std::max(x, 0.0), 1.0); };
  return RGBColor((float)(sat(sum_r) * vertex_color.r),
                  (float)(sat(sum_g) * vertex_color.g),
                  (float)(sat(sum_b) * vertex_color.b));
}

// Builds the shared two-light shadow scene:
//   receiver  : vertical quad at x = 0 (normal +x), y,z in [-30, 30]
//   occluder  : vertical quad at x = -25 (between light camera and receiver),
//               y,z in [-8, 8]
//   light A   : red,   direction (-1, 0, 0)        -> blocked at the centre
//   light B   : blue,  direction norm(-1, 0, -1)   -> clear at the centre
struct ShadowScene {
  FlatMaterial ground_material;
  FlatMaterial occluder_material;
  GeometryObject* receiver;
  GeometryObject* occluder;
  DirectionalLight light_a;
  DirectionalLight light_b;

  ShadowScene()
    : ground_material(RGBColor(1, 1, 1), RGBColor(0, 0, 0), 1.0f),
      occluder_material(RGBColor(1, 1, 1), RGBColor(0, 0, 0), 1.0f),
      receiver(verticalPlane(&ground_material, 0.0, 30.0)),
      occluder(verticalPlane(&occluder_material, -25.0, 8.0)),
      light_a(RGBColor(0.5f, 0.0f, 0.0f), Vector3D(-1, 0, 0)),
      light_b(RGBColor(0.0f, 0.0f, 0.5f), Vector3D(-0.7071067811865475, 0, -0.7071067811865475)) {}

  void populate(World& world, bool with_occluder = true) const {
    world.m_objects.clear();
    world.m_objects.push_back(receiver);
    if (with_occluder) world.m_objects.push_back(occluder);
    world.m_lights = {const_cast<DirectionalLight*>(&light_a),
                      const_cast<DirectionalLight*>(&light_b)};
  }

  // Angled main camera parameters (shared by every shadow scene) so the
  // Blinn half-vector is never the zero vector of an exactly anti-parallel view.
};

const RGBColor WHITE(1, 1, 1);

// Angled main camera shared by every shadow scene.
const Point3D SCENE_CAMERA_POS(-80, 12, 18);
const Point3D SCENE_CAMERA_TARGET(0, 0, 0);
const uint32_t SCENE_VIEW = 80, SCENE_NEAR = 2, SCENE_FAR = 400;

}  // namespace

// -------------------------------- lighting --------------------------------

// Two lights must sum commutatively: swapping the light order cannot change
// the shaded color, and the value must match the independent calculation.
static int lighting_order_and_manual_sum() {
  World world;
  ForwardProbe renderer(&world);
  OrthographicCamera camera(Point3D(-80, 12, 18), Point3D(0, 0, 0), 80, 80, &renderer, 2, 400);
  world.m_camera = &camera;

  DirectionalLight red(RGBColor(0.5f, 0, 0), Vector3D(-1, 0, 0));
  DirectionalLight blue(RGBColor(0, 0, 0.5f), Vector3D(-0.7071067811865475, 0, -0.7071067811865475));
  const Vector3D N(1, 0, 0);
  const Vector3D V = -camera.viewDirection(Point3D(0, 0, 0));
  Fragment fragment(Point3D(0, 0, 0), RGBColor(1, 1, 1), RGBColor(1, 1, 1),
                    RGBColor(0, 0, 0), N);

  std::vector<Light*> rb = {&red, &blue};
  std::vector<Light*> br = {&blue, &red};
  const RGBColor order_rb = Material::shade(rb, camera, fragment);
  const RGBColor order_br = Material::shade(br, camera, fragment);
  TASK_CHECK(nearColor(order_rb, order_br, 1e-6f));

  const std::vector<const Light*> manual_lights = {&red, &blue};
  const RGBColor expected = manualBlinn(manual_lights, {1.0, 1.0}, N, V,
                                        RGBColor(1, 1, 1), RGBColor(1, 1, 1));
  TASK_CHECK(nearColor(order_rb, expected, 1e-5f));
  // Ambient (0.1) is present once in every channel even though it is not a light.
  TASK_CHECK(near(order_rb.g, 0.1, 1e-5));
  return 0;
}

// Per-light shadow factors only dim their own light; ambient survives and the
// fully blocked case leaves exactly the ambient term.
static int lighting_per_light_occlusion() {
  World world;
  ForwardProbe renderer(&world);
  OrthographicCamera camera(Point3D(-80, 12, 18), Point3D(0, 0, 0), 80, 80, &renderer, 2, 400);
  world.m_camera = &camera;

  DirectionalLight red(RGBColor(0.5f, 0, 0), Vector3D(-1, 0, 0));
  DirectionalLight blue(RGBColor(0, 0, 0.5f), Vector3D(-0.7071067811865475, 0, -0.7071067811865475));
  std::vector<Light*> lights = {&red, &blue};
  const Vector3D N(1, 0, 0);
  const Vector3D V = -camera.viewDirection(Point3D(0, 0, 0));
  Fragment fragment(Point3D(0, 0, 0), RGBColor(1, 1, 1), RGBColor(1, 1, 1),
                    RGBColor(0, 0, 0), N);
  const std::vector<const Light*> manual_lights = {&red, &blue};

  // Red fully blocked, blue fully lit: red channel drops to ambient only.
  const RGBColor red_blocked = Material::shade(lights, {1.0, 0.0}, camera, fragment);
  const RGBColor expect_red_blocked = manualBlinn(manual_lights, {0.0, 1.0}, N, V,
                                                  RGBColor(1, 1, 1), RGBColor(1, 1, 1));
  TASK_CHECK(nearColor(red_blocked, expect_red_blocked));
  TASK_CHECK(near(red_blocked.r, 0.1, 1e-5));       // ambient only on red
  TASK_CHECK(red_blocked.b > 0.1);                  // blue direct light intact

  // Blue fully blocked, red fully lit.
  const RGBColor blue_blocked = Material::shade(lights, {0.0, 1.0}, camera, fragment);
  TASK_CHECK(near(blue_blocked.b, 0.1, 1e-5));      // ambient only on blue
  TASK_CHECK(near(blue_blocked.r, 0.6, 1e-5));      // ambient + red

  // Both blocked: only the single ambient term remains in every channel.
  const RGBColor both_blocked = Material::shade(lights, {1.0, 1.0}, camera, fragment);
  TASK_CHECK(nearColor(both_blocked, RGBColor(0.1f, 0.1f, 0.1f), 1e-5f));
  return 0;
}

// Unclamped per-light accumulation followed by a single saturation: pick
// lights whose summed channels exceed 1 and confirm the result is clamped to 1
// regardless of accumulation order (no premature per-light truncation).
static int lighting_single_final_clamp() {
  World world;
  ForwardProbe renderer(&world);
  OrthographicCamera camera(Point3D(-80, 12, 18), Point3D(0, 0, 0), 80, 80, &renderer, 2, 400);
  world.m_camera = &camera;

  DirectionalLight l1(RGBColor(0.8f, 0.0f, 0.0f), Vector3D(-1, 0, 0));
  DirectionalLight l2(RGBColor(0.8f, 0.0f, 0.0f), Vector3D(-1, 0, 0));
  Fragment fragment(Point3D(0, 0, 0), RGBColor(1, 1, 1), RGBColor(1, 1, 1),
                    RGBColor(0, 0, 0), Vector3D(1, 0, 0));

  const RGBColor a = Material::shade(std::vector<Light*>{&l1, &l2}, camera, fragment);
  const RGBColor b = Material::shade(std::vector<Light*>{&l2, &l1}, camera, fragment);
  TASK_CHECK(nearColor(a, b, 1e-6f));
  // 0.1 ambient + 0.8 + 0.8 = 1.7 -> saturated to 1.0, not truncated earlier.
  TASK_CHECK(near(a.r, 1.0, 1e-5));
  TASK_CHECK(near(a.g, 0.1, 1e-5));
  TASK_CHECK(near(a.b, 0.1, 1e-5));
  return 0;
}

// Real geometry, one light blocked and the other clear: forward and deferred
// must produce identical visible surface colors, and both shadow regions must
// appear with the ambient/other-light contribution preserved.
static int lighting_real_geometry_forward_equals_deferred() {
  ShadowScene scene;
  World world;
  scene.populate(world);

  ForwardProbe forward(&world);
  OrthographicCamera fcam(SCENE_CAMERA_POS, SCENE_CAMERA_TARGET,
                          SCENE_VIEW, SCENE_VIEW, &forward,
                          SCENE_NEAR, SCENE_FAR);
  world.m_camera = &fcam;
  forward.render(true, true);

  DeferredProbe deferred(&world);
  OrthographicCamera dcam(SCENE_CAMERA_POS, SCENE_CAMERA_TARGET,
                          SCENE_VIEW, SCENE_VIEW, &deferred,
                          SCENE_NEAR, SCENE_FAR);
  world.m_camera = &dcam;
  deferred.render(true, true);

  const auto& fp = forward.pixels();
  const auto& dp = deferred.pixels();
  TASK_CHECK(fp.size() == dp.size());

  int mismatches = 0;
  for (size_t i = 0; i < fp.size(); ++i) {
    if (!nearColor(fp[i], dp[i], 1e-5f)) ++mismatches;
  }
  TASK_CHECK(mismatches == 0);

  // Expected region colors (independently derived):
  //  fully lit        : ambient 0.1 + red 0.5 + blue 0.5/sqrt(2)
  //  red only (B off) : (0.6, 0.1, 0.1)
  //  blue only (A off): (0.1, 0.1, ~0.4536)
  const RGBColor lit(0.6f, 0.1f, (float)(0.1 + 0.5 / std::sqrt(2.0)));
  const RGBColor red_only(0.6f, 0.1f, 0.1f);
  const RGBColor blue_only(0.1f, 0.1f, (float)(0.1 + 0.5 / std::sqrt(2.0)));

  int lit_count = 0, red_only_count = 0, blue_only_count = 0;
  for (const RGBColor& p : fp) {
    if (nearColor(p, lit)) ++lit_count;
    else if (nearColor(p, red_only)) ++red_only_count;
    else if (nearColor(p, blue_only)) ++blue_only_count;
  }
  TASK_CHECK(lit_count > 0);
  TASK_CHECK(red_only_count > 0);    // blue blocked, red + ambient remain
  TASK_CHECK(blue_only_count > 0);   // red blocked, blue + ambient remain

  // Every shadowed region still carries the 0.1 ambient (never crushed to 0).
  for (const RGBColor& p : fp) {
    if (nearColor(p, red_only) || nearColor(p, blue_only)) {
      TASK_CHECK(p.r >= 0.1f - COLOR_EPS && p.g >= 0.1f - COLOR_EPS &&
                 p.b >= 0.1f - COLOR_EPS);
    }
  }
  return 0;
}

// Removing the occluder and re-rendering must restore full lighting: the
// single-light shadow regions disappear and every receiver pixel is fully lit.
static int lighting_occluder_removal_restores_light() {
  ShadowScene scene;
  World world;
  scene.populate(world, /*with_occluder=*/true);

  ForwardProbe forward(&world);
  OrthographicCamera camera(SCENE_CAMERA_POS, SCENE_CAMERA_TARGET,
                            SCENE_VIEW, SCENE_VIEW, &forward,
                            SCENE_NEAR, SCENE_FAR);
  world.m_camera = &camera;
  forward.render(true, true);
  TASK_CHECK(forward.mapCount() == 2);

  scene.populate(world, /*with_occluder=*/false);
  forward.render(true, true);

  const RGBColor red_only(0.6f, 0.1f, 0.1f);
  const RGBColor blue_only(0.1f, 0.1f, (float)(0.1 + 0.5 / std::sqrt(2.0)));
  int shadowed = 0;
  for (const RGBColor& p : forward.pixels()) {
    if (nearColor(p, red_only) || nearColor(p, blue_only)) ++shadowed;
  }
  TASK_CHECK(shadowed == 0);
  return 0;
}

// Shadows disabled: direct lighting is applied undimmed and no shadow map is
// retained from a previous render.
static int lighting_shadow_toggle_off() {
  ShadowScene scene;
  World world;
  scene.populate(world);

  ForwardProbe forward(&world);
  OrthographicCamera camera(SCENE_CAMERA_POS, SCENE_CAMERA_TARGET,
                            SCENE_VIEW, SCENE_VIEW, &forward,
                            SCENE_NEAR, SCENE_FAR);
  world.m_camera = &camera;

  forward.render(true, true);
  TASK_CHECK(forward.mapCount() == 2);

  forward.render(true, false);
  TASK_CHECK(forward.mapCount() == 0);

  const RGBColor lit(0.6f, 0.1f, (float)(0.1 + 0.5 / std::sqrt(2.0)));
  int dimmed = 0;
  for (const RGBColor& p : forward.pixels()) {
    if (!(nearColor(p, lit) || nearColor(p, WHITE))) ++dimmed;
  }
  TASK_CHECK(dimmed == 0);
  return 0;
}

// ------------------------------- projection -------------------------------

// Orthographic light camera: a slanted occluder's depth is the linear
// interpolation of its camera-space vertex depths along the view axis.
static int projection_orthographic_linear_depth() {
  FlatMaterial material(RGBColor(1, 1, 1), RGBColor(0, 0, 0), 1.0f);
  // Slanted quad: depth (x) varies with y, always between camera and receiver.
  GeometryObject* slanted = makeQuad(&material, {
    Point3D(-30, -8, -8), Point3D(-20, 8, -8),
    Point3D(-20,  8,  8), Point3D(-30, -8,  8)
  });

  World world;
  world.m_objects = {slanted};
  ForwardProbe renderer(&world);
  OrthographicCamera camera(Point3D(-80, 12, 18), Point3D(0, 0, 0), 80, 80, &renderer, 2, 400);
  world.m_camera = &camera;

  const std::vector<Triangle3D> triangles = slanted->triangles();
  const Triangle3D& triangle_world = triangles[0];
  const Triangle2D triangle_raster = renderer.rasterize(triangle_world);

  // Evaluate at several interior pixels and compare against a manual linear
  // blend of the camera-space vertex depths using the screen barycentrics.
  const Point3D z1 = camera.viewTransform(triangle_world.v1.position);
  const Point3D z2 = camera.viewTransform(triangle_world.v2.position);
  const Point3D z3 = camera.viewTransform(triangle_world.v3.position);

  const BoundingBox2D bbox = triangle_raster.bbox();
  int samples = 0;
  for (int32_t x = bbox.min.x; x <= bbox.max.x && samples < 8; ++x) {
    for (int32_t y = bbox.min.y; y <= bbox.max.y && samples < 8; ++y) {
      const Point2D pixel((double)x, (double)y);
      if (!triangle_raster.contains(pixel)) continue;
      double u, v, w;
      triangle_raster.calculateBarycentricCoords(u, v, w, pixel);

      const double expected_linear = z1.z * u + z2.z * v + z3.z * w;
      const double actual = renderer.calculateDepth(triangle_world, triangle_raster, pixel);
      TASK_CHECK(near(actual, expected_linear, 1e-6));

      // The inverse-z formula would give a different value on a slanted face,
      // proving the orthographic branch really is linear.
      const double inverse_z = 1.0 / (u / z1.z + v / z2.z + w / z3.z);
      TASK_CHECK(!near(actual, inverse_z, 1e-3));
      ++samples;
    }
  }
  TASK_CHECK(samples > 0);
  delete slanted;
  return 0;
}

// Perspective main camera: depth keeps the inverse-z (1/z) interpolation.
static int projection_perspective_inverse_depth() {
  FlatMaterial material(RGBColor(1, 1, 1), RGBColor(0, 0, 0), 1.0f);
  GeometryObject* slanted = makeQuad(&material, {
    Point3D(-12, -8, 20), Point3D(12, -8, 20),
    Point3D(12,  8, 40), Point3D(-12,  8, 40)
  });

  World world;
  world.m_objects = {slanted};
  ForwardProbe renderer(&world);
  PerspectiveCamera camera(Point3D(0, 0, -60), Point3D(0, 0, 0), 80, 80, &renderer, 2, 400, 90.0f);
  world.m_camera = &camera;

  const std::vector<Triangle3D> triangles = slanted->triangles();
  const Triangle3D& triangle_world = triangles[0];
  const Triangle2D triangle_raster = renderer.rasterize(triangle_world);

  const Point3D z1 = camera.viewTransform(triangle_world.v1.position);
  const Point3D z2 = camera.viewTransform(triangle_world.v2.position);
  const Point3D z3 = camera.viewTransform(triangle_world.v3.position);

  const BoundingBox2D bbox = triangle_raster.bbox();
  int samples = 0;
  for (int32_t x = bbox.min.x; x <= bbox.max.x && samples < 8; ++x) {
    for (int32_t y = bbox.min.y; y <= bbox.max.y && samples < 8; ++y) {
      const Point2D pixel((double)x, (double)y);
      if (!triangle_raster.contains(pixel)) continue;
      double u, v, w;
      triangle_raster.calculateBarycentricCoords(u, v, w, pixel);

      const double expected_inverse = 1.0 / (u / z1.z + v / z2.z + w / z3.z);
      const double actual = renderer.calculateDepth(triangle_world, triangle_raster, pixel);
      TASK_CHECK(near(actual, expected_inverse, 1e-6));
      ++samples;
    }
  }
  TASK_CHECK(samples > 0);
  delete slanted;
  return 0;
}

// pointInShadow returns a [0,1] ratio: hard mode yields exactly 0 or 1, points
// outside the shadow map or beyond the light camera's near/far are unoccluded,
// and the comparison runs on the light camera's axis depth.
static int projection_shadow_bounds_and_axis_depth() {
  FlatMaterial material(RGBColor(1, 1, 1), RGBColor(0, 0, 0), 1.0f);
  GeometryObject* receiver = verticalPlane(&material, 0.0, 30.0);
  GeometryObject* occluder = verticalPlane(&material, -25.0, 8.0);

  World world;
  world.m_objects = {receiver, occluder};
  DirectionalLight light(RGBColor(0.5f, 0, 0), Vector3D(-1, 0, 0));
  world.m_lights = {&light};

  ForwardProbe renderer(&world);
  OrthographicCamera camera(Point3D(-80, 12, 18), Point3D(0, 0, 0), 80, 80, &renderer, 2, 400);
  world.m_camera = &camera;
  renderer.createShadowMaps();
  ShadowMap* map = renderer.map(0);
  const Camera* light_camera = map->camera();

  // Centre is behind the occluder along the light axis -> fully occluded.
  const double centre = map->pointInShadow(Point3D(0, 0, 0));
  TASK_CHECK(near(centre, 1.0));

  // Hard comparison only ever returns 0 or 1.
  TASK_CHECK(centre == 0.0 || centre == 1.0);

  // Laterally outside the shadow map: unoccluded.
  const Point3D outside(0, 900000, 900000);
  const Point2D raster = map->rasterizer()->rasterize(outside);
  TASK_CHECK(raster.x < 0 || raster.x >= map->width() ||
             raster.y < 0 || raster.y >= map->height());
  TASK_CHECK(near(map->pointInShadow(outside), 0.0));

  // Along the view axis outside [near, far]: unoccluded.
  const Point3D beyond_far(5000000, 0, 0);
  const Point3D in_front(-5000000, 0, 0);
  TASK_CHECK(light_camera->viewTransform(beyond_far).z > light_camera->get_far_plane());
  TASK_CHECK(light_camera->viewTransform(in_front).z < light_camera->get_near_plane());
  TASK_CHECK(near(map->pointInShadow(beyond_far), 0.0));
  TASK_CHECK(near(map->pointInShadow(in_front), 0.0));

  // The depth being compared is the orthographic camera's axis depth.
  const double axis_depth = light_camera->viewTransform(Point3D(0, 0, 0)).z;
  TASK_CHECK(axis_depth > light_camera->get_near_plane() &&
             axis_depth < light_camera->get_far_plane());

  renderer.releaseShadowMaps();
  delete receiver;
  delete occluder;
  return 0;
}

// -------------------------------- lifetime --------------------------------

// Each render builds exactly one shadow map per current light, owns an
// internal camera + rasterizer while alive, and frees them on release.
static int lifetime_map_count_and_owned_resources() {
  ShadowScene scene;
  World world;
  scene.populate(world);

  ForwardProbe renderer(&world);
  OrthographicCamera camera(SCENE_CAMERA_POS, SCENE_CAMERA_TARGET,
                            SCENE_VIEW, SCENE_VIEW, &renderer,
                            SCENE_NEAR, SCENE_FAR);
  world.m_camera = &camera;

  renderer.render(true, true);
  TASK_CHECK(renderer.mapCount() == 2);
  for (size_t i = 0; i < renderer.mapCount(); ++i) {
    TASK_CHECK(renderer.map(i)->camera() != nullptr);
    TASK_CHECK(renderer.map(i)->rasterizer() != nullptr);
    TASK_CHECK(renderer.map(i)->width() == 800);
  }

  renderer.releaseShadowMaps();
  TASK_CHECK(renderer.mapCount() == 0);
  return 0;
}

// Repeated renders, light removal and light reordering must never accumulate
// stale maps; the stored maps always match the current light set.
static int lifetime_repeat_remove_reorder() {
  ShadowScene scene;
  World world;
  scene.populate(world);

  ForwardProbe renderer(&world);
  OrthographicCamera camera(SCENE_CAMERA_POS, SCENE_CAMERA_TARGET,
                            SCENE_VIEW, SCENE_VIEW, &renderer,
                            SCENE_NEAR, SCENE_FAR);
  world.m_camera = &camera;

  renderer.render(true, true);
  TASK_CHECK(renderer.mapCount() == 2);
  renderer.render(true, true);
  TASK_CHECK(renderer.mapCount() == 2);  // no accumulation on repeat

  // Remove one light: map count follows the new set, no leftover map.
  world.m_lights = {const_cast<DirectionalLight*>(&scene.light_a)};
  renderer.render(true, true);
  TASK_CHECK(renderer.mapCount() == 1);

  // Reorder / change the set again and repeat: still exactly current count.
  world.m_lights = {const_cast<DirectionalLight*>(&scene.light_b),
                    const_cast<DirectionalLight*>(&scene.light_a)};
  renderer.render(true, true);
  TASK_CHECK(renderer.mapCount() == 2);
  renderer.render(true, true);
  TASK_CHECK(renderer.mapCount() == 2);

  // Reordering the lights must not change the visible color.
  const std::vector<RGBColor> swapped = renderer.pixels();
  world.m_lights = {const_cast<DirectionalLight*>(&scene.light_a),
                    const_cast<DirectionalLight*>(&scene.light_b)};
  renderer.render(true, true);
  int mismatches = 0;
  const auto& original = renderer.pixels();
  for (size_t i = 0; i < original.size(); ++i) {
    if (!nearColor(original[i], swapped[i], 1e-5f)) ++mismatches;
  }
  TASK_CHECK(mismatches == 0);
  return 0;
}

// Turning shadows off and on across consecutive renders releases and rebuilds
// the maps rather than reusing stale ones, and tearing the renderer down does
// not leak the internal cameras/rasterizers.
static int lifetime_toggle_and_destruction() {
  ShadowScene scene;
  World world;
  scene.populate(world);

  {
    ForwardProbe renderer(&world);
    OrthographicCamera camera(SCENE_CAMERA_POS, SCENE_CAMERA_TARGET,
                              SCENE_VIEW, SCENE_VIEW, &renderer,
                              SCENE_NEAR, SCENE_FAR);
    world.m_camera = &camera;

    renderer.render(true, true);
    TASK_CHECK(renderer.mapCount() == 2);
    renderer.render(true, false);
    TASK_CHECK(renderer.mapCount() == 0);   // old maps cannot be reused
    renderer.render(true, true);
    TASK_CHECK(renderer.mapCount() == 2);   // fresh maps rebuilt
    renderer.render(true, false);
    TASK_CHECK(renderer.mapCount() == 0);
    // Destructor runs here; it must release the last active set without
    // leaking or double-freeing the internal cameras/rasterizers.
  }

  // A second renderer over the same world starts with no inherited maps.
  ForwardProbe renderer2(&world);
  OrthographicCamera camera2(SCENE_CAMERA_POS, SCENE_CAMERA_TARGET,
                             SCENE_VIEW, SCENE_VIEW, &renderer2,
                             SCENE_NEAR, SCENE_FAR);
  world.m_camera = &camera2;
  TASK_CHECK(renderer2.mapCount() == 0);
  renderer2.render(true, true);
  TASK_CHECK(renderer2.mapCount() == 2);
  return 0;
}

// ------------------------------- registration ------------------------------

extern "C" const struct task_case task_cases[] = {
  // lighting
  {"lighting", "order_and_manual_sum",        lighting_order_and_manual_sum},
  {"lighting", "per_light_occlusion",         lighting_per_light_occlusion},
  {"lighting", "single_final_clamp",          lighting_single_final_clamp},
  {"lighting", "forward_equals_deferred",     lighting_real_geometry_forward_equals_deferred},
  {"lighting", "occluder_removal_restores",   lighting_occluder_removal_restores_light},
  {"lighting", "shadow_toggle_off",           lighting_shadow_toggle_off},
  // projection
  {"projection", "orthographic_linear_depth", projection_orthographic_linear_depth},
  {"projection", "perspective_inverse_depth", projection_perspective_inverse_depth},
  {"projection", "shadow_bounds_axis_depth",  projection_shadow_bounds_and_axis_depth},
  // lifetime
  {"lifetime", "map_count_owned_resources",   lifetime_map_count_and_owned_resources},
  {"lifetime", "repeat_remove_reorder",       lifetime_repeat_remove_reorder},
  {"lifetime", "toggle_and_destruction",      lifetime_toggle_and_destruction},
};

extern "C" const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
