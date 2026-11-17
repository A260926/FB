// Regression tests for multi-light direct lighting, per-light shadowing and
// shadow-resource ownership. Runs headless: it drives the production
// rasterizers with real geometry and reads the protected buffers through
// derived probe classes.
#include "task_support/check.h"

#include "DeferredRasterizer.h"
#include "DIRECTIONALLIGHT.H"
#include "ForwardRasterizer.h"
#include "Material.h"
#include "OrthographicCamera.h"
#include "PERSPECTIVECAMERA.H"
#include "PointLight.h"
#include "ShadowMap.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

// ---------------------------------------------------------------- helpers

class ForwardProbe : public ForwardRasterizer {
public:
  ForwardProbe(const World* world) : ForwardRasterizer(world) {}

  const std::vector<RGBColor>& pixels() const { return m_pixels; }
  const std::vector<double>& depths() const { return m_depth_buffer; }
  size_t shadowMapCount() const { return m_shadow_maps.size(); }
  const ShadowMap* shadowMap(size_t i) const { return m_shadow_maps[i]; }
  std::vector<double> factors(const Point3D& p) const { return shadowFactors(p); }
};

class DeferredProbe : public DeferredRasterizer {
public:
  DeferredProbe(const World* world) : DeferredRasterizer(world) {}

  const std::vector<RGBColor>& pixels() const { return m_pixels; }
  const std::vector<double>& depths() const { return m_depth_buffer; }
  size_t shadowMapCount() const { return m_shadow_maps.size(); }
  const ShadowMap* shadowMap(size_t i) const { return m_shadow_maps[i]; }
  std::vector<double> factors(const Point3D& p) const { return shadowFactors(p); }
};

// Flat quad in the XY plane. Winding (0,2,1),(0,3,2) yields a +Z normal.
GeometryObject* makeQuad(Material* material, const Point3D& a, const Point3D& b,
                         const Point3D& c, const Point3D& d) {
  std::vector<Point3D> vertices{ a, b, c, d };
  std::vector<RGBColor> colors(4, RGBColor(1.0f, 1.0f, 1.0f));
  std::vector<Vector2D> texture_coords(4, Vector2D(0.0, 0.0));
  std::vector<uint32_t> indices{ 0, 2, 1, 0, 3, 2 };
  return new GeometryObject(material, vertices, colors, texture_coords, indices, Point3D(0, 0, 0));
}

// Ground quad with the same upward-facing winding as makeQuad.
GeometryObject* makeGround(Material* material, const double half_extent) {
  const double h = half_extent;
  return makeQuad(material,
                  Point3D(-h, -h, 0), Point3D(h, -h, 0),
                  Point3D(h, h, 0), Point3D(-h, h, 0));
}

bool approx(const double actual, const double expected, const double tolerance) {
  return std::fabs(actual - expected) <= tolerance;
}

// Index of the pixel whose pixel-centre contains `point`.
int pixelIndexOf(const Rasterizer& rasterizer, const Camera& camera, const Point3D& point) {
  const Point2D raster = rasterizer.rasterize(point);
  const int x = (int)std::floor(raster.x);
  const int y = (int)std::floor(raster.y);
  if (x < 0 || y < 0 || x >= (int)camera.get_width() || y >= (int)camera.get_height()) {
    return -1;
  }
  return y * camera.get_width() + x;
}

// ---------------------------------------------------------------- lighting

// Two directional lights and ambient; no occluders, so every fragment is lit
// by both. Everything here is computed by hand from the formula, independent
// of the production implementation:
//   color = clamp(ambient + sum_light(light_color * (diffuse * NdotL
//                                                    + specular * (H dot V)^s)),
//                 [0,1]) * vertex_color
// Lights point along -Z, the normal and view are +Z, so NdotL = H dot V = 1.
// With light A = (0.4, 0, 0), light B = (0, 0, 0.3), diffuse = specular = 0.5
// and a white vertex color:
//   r = 0.1 + 0.4 * (0.5 + 0.5) = 0.5
//   g = 0.1
//   b = 0.1 + 0.3 * (0.5 + 0.5) = 0.4
const double TWO_LIGHT_EXPECTED_R = 0.5;
const double TWO_LIGHT_EXPECTED_G = 0.1;
const double TWO_LIGHT_EXPECTED_B = 0.4;

int test_lighting_order_independence() {
  FlatMaterial material(RGBColor(0.5f, 0.5f, 0.5f), RGBColor(0.5f, 0.5f, 0.5f), 1.0f);
  DirectionalLight red(RGBColor(0.4f, 0.0f, 0.0f), Vector3D(0, 0, -1));
  DirectionalLight blue(RGBColor(0.0f, 0.0f, 0.3f), Vector3D(0, 0, -1));

  World world;
  ForwardProbe forward(&world);
  PerspectiveCamera camera(Point3D(0, 0, 10), Point3D(0, 0, 0), 16, 16, &forward, 2, 100);
  world.m_camera = &camera;

  // A single fragment in the middle of the screen, lit by both lights.
  Fragment fragment(Point3D(0, 0, 0), RGBColor(1.0f, 1.0f, 1.0f),
                    RGBColor(0.5f, 0.5f, 0.5f), RGBColor(0.5f, 0.5f, 0.5f),
                    Vector3D(0, 0, 1));

  const RGBColor with_red_blue = Material::shade({ &red, &blue }, camera, fragment);
  const RGBColor with_blue_red = Material::shade({ &blue, &red }, camera, fragment);

  // The order of the lights must not change the resulting color.
  TASK_CHECK(approx(with_red_blue.r, with_blue_red.r, 1e-6));
  TASK_CHECK(approx(with_red_blue.g, with_blue_red.g, 1e-6));
  TASK_CHECK(approx(with_red_blue.b, with_blue_red.b, 1e-6));

  TASK_CHECK(approx(with_red_blue.r, TWO_LIGHT_EXPECTED_R, 1e-5));
  TASK_CHECK(approx(with_red_blue.g, TWO_LIGHT_EXPECTED_G, 1e-5));
  TASK_CHECK(approx(with_red_blue.b, TWO_LIGHT_EXPECTED_B, 1e-5));
  return 0;
}

int test_lighting_accumulates_per_light() {
  // Direct light must sum per light (not keep only the last one) and ambient
  // must be added exactly once.
  FlatMaterial material(RGBColor(0.5f, 0.5f, 0.5f), RGBColor(0.0f, 0.0f, 0.0f), 1.0f);
  DirectionalLight red(RGBColor(0.4f, 0.0f, 0.0f), Vector3D(0, 0, -1));
  DirectionalLight blue(RGBColor(0.0f, 0.0f, 0.3f), Vector3D(0, 0, -1));

  World world;
  ForwardProbe forward(&world);
  PerspectiveCamera camera(Point3D(0, 0, 10), Point3D(0, 0, 0), 16, 16, &forward, 2, 100);
  world.m_camera = &camera;

  Fragment fragment(Point3D(0, 0, 0), RGBColor(1.0f, 1.0f, 1.0f),
                    RGBColor(0.5f, 0.5f, 0.5f), RGBColor(0.0f, 0.0f, 0.0f),
                    Vector3D(0, 0, 1));

  const RGBColor two = Material::shade({ &red, &blue }, camera, fragment);
  // Ambient is 0.1 exactly once; each light contributes color*0.5.
  TASK_CHECK(approx(two.r, 0.1 + 0.4 * 0.5, 1e-6));
  TASK_CHECK(approx(two.g, 0.1, 1e-6));
  TASK_CHECK(approx(two.b, 0.1 + 0.3 * 0.5, 1e-6));

  // Dropping the blue light must reduce only the blue channel.
  const RGBColor one = Material::shade({ &red }, camera, fragment);
  TASK_CHECK(approx(one.r, 0.1 + 0.4 * 0.5, 1e-6));
  TASK_CHECK(approx(one.b, 0.1, 1e-6));
  return 0;
}

int test_lighting_clamps_before_vertex_color() {
  // Sum the lights in an unclamped accumulator and only clamp the per-channel
  // total before multiplying by the vertex color. Vertex color is 0.5, so a
  // pre-clamp (which would cap the light color at 1.0 before adding), or a
  // post-multiply clamp, would give a different result.
  FlatMaterial material(RGBColor(0.5f, 0.5f, 0.5f), RGBColor(0.0f, 0.0f, 0.0f), 1.0f);
  DirectionalLight bright_a(RGBColor(0.8f, 0.0f, 0.0f), Vector3D(0, 0, -1));
  DirectionalLight bright_b(RGBColor(0.8f, 0.0f, 0.0f), Vector3D(0, 0, -1));

  World world;
  ForwardProbe forward(&world);
  PerspectiveCamera camera(Point3D(0, 0, 10), Point3D(0, 0, 0), 16, 16, &forward, 2, 100);
  world.m_camera = &camera;

  Fragment fragment(Point3D(0, 0, 0), RGBColor(1.0f, 1.0f, 1.0f),
                    RGBColor(1.0f, 1.0f, 1.0f), RGBColor(0.0f, 0.0f, 0.0f),
                    Vector3D(0, 0, 1));

  const RGBColor color = Material::shade({ &bright_a, &bright_b }, camera, fragment);
  // total = 0.1 + 0.8 + 0.8 = 1.7 -> clamp to 1.0.
  TASK_CHECK(approx(color.r, 1.0, 1e-6));
  // No light reaches green/blue: only ambient 0.1.
  TASK_CHECK(approx(color.g, 0.1, 1e-6));
  TASK_CHECK(approx(color.b, 0.1, 1e-6));
  return 0;
}

// -------------------------------------------------------------- projection

int test_projection_orthographic_linear_depth() {
  World world;
  ForwardProbe forward(&world);
  OrthographicCamera camera(Point3D(0, 0, 100), Point3D(0, 0, 0), 200, 200, &forward, 2, 1000);
  world.m_camera = &camera;

  // A slanted triangle whose camera-space depths are 90, 40, 40 z units from
  // the camera. Under an orthographic projection the axial depth at the
  // screen-space centroid is the linear (affine) interpolation of the vertex
  // depths, not the inverse-depth interpolation used for perspective.
  const Triangle3D triangle(
    Vertex3D(Point3D(-40, -40, 10), RGBColor(), Vector2D(), Vector3D(0, 0, 1)),
    Vertex3D(Point3D(40, 40, 60), RGBColor(), Vector2D(), Vector3D(0, 0, 1)),
    Vertex3D(Point3D(40, -40, 60), RGBColor(), Vector2D(), Vector3D(0, 0, 1)));

  const Triangle2D screen = forward.rasterize(triangle);
  const Point2D centroid((screen.v1.x + screen.v2.x + screen.v3.x) / 3.0,
                         (screen.v1.y + screen.v2.y + screen.v3.y) / 3.0);

  double u, v, w;
  screen.calculateBarycentricCoords(u, v, w, centroid);

  const double depth = forward.calculateDepth(triangle, screen, centroid);
  const double linear = 90.0 * u + 40.0 * v + 40.0 * w;
  const double inverse = 1.0 / ((1.0 / 90.0) * u + (1.0 / 40.0) * v + (1.0 / 40.0) * w);

  TASK_CHECK(approx(depth, linear, 1e-6));
  TASK_CHECK(!approx(depth, inverse, 1e-3));
  return 0;
}

int test_projection_perspective_inverse_depth() {
  World world;
  ForwardProbe forward(&world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 200, 200, &forward, 1, 1000, 90.0f);
  world.m_camera = &camera;

  const Triangle3D triangle(
    Vertex3D(Point3D(-20, -20, 20), RGBColor(), Vector2D(), Vector3D(0, 0, 1)),
    Vertex3D(Point3D(20, 20, 20), RGBColor(), Vector2D(), Vector3D(0, 0, 1)),
    Vertex3D(Point3D(20, -20, 80), RGBColor(), Vector2D(), Vector3D(0, 0, 1)));

  const Triangle2D screen = forward.rasterize(triangle);
  const Point2D centroid((screen.v1.x + screen.v2.x + screen.v3.x) / 3.0,
                         (screen.v1.y + screen.v2.y + screen.v3.y) / 3.0);

  double u, v, w;
  screen.calculateBarycentricCoords(u, v, w, centroid);

  const double depth = forward.calculateDepth(triangle, screen, centroid);
  const double inverse = 1.0 / ((1.0 / 20.0) * u + (1.0 / 20.0) * v + (1.0 / 80.0) * w);

  TASK_CHECK(approx(depth, inverse, 1e-4));
  return 0;
}

int test_projection_tilted_occluder_shadow() {
  // A tilted occluder between a point light and the ground. Under the light's
  // orthographic camera the shadow band shifts with the receiver's position,
  // which only happens if the occluder depth is interpolated linearly in
  // screen space. Somewhere along the band the shadow must be cast.
  FlatMaterial material(RGBColor(0.6f, 0.6f, 0.6f), RGBColor(0.1f, 0.1f, 0.1f), 1.0f);
  GeometryObject* ground = makeGround(&material, 400.0);
  GeometryObject* tilted = makeQuad(&material,
                                    Point3D(-120, 150, 180), Point3D(120, 150, 180),
                                    Point3D(120, 250, 60), Point3D(-120, 250, 60));

  World world;
  world.m_objects = { ground, tilted };
  PointLight light(RGBColor(0.4f, 0.4f, 0.4f), Point3D(0, 0, 300));
  ShadowMap shadow_map(&world, &light);
  shadow_map.render();

  ShadowMap::pcfShadows() = false;
  TASK_CHECK(approx(shadow_map.pointInShadow(Point3D(0, 100, 0)), 0.0, 1e-6));
  TASK_CHECK(approx(shadow_map.pointInShadow(Point3D(0, 200, 0)), 1.0, 1e-6));
  TASK_CHECK(approx(shadow_map.pointInShadow(Point3D(0, 300, 0)), 0.0, 1e-6));
  return 0;
}

int test_projection_edge_and_outside_is_lit() {
  FlatMaterial material(RGBColor(0.6f, 0.6f, 0.6f), RGBColor(0.1f, 0.1f, 0.1f), 1.0f);
  GeometryObject* ground = makeGround(&material, 400.0);
  // A small occluder flush with the bottom edge of the light's orthographic
  // view, so a receiver just under it lands on the shadow map's edge row.
  GeometryObject* border = makeQuad(&material,
                                    Point3D(-200, -320, 40), Point3D(2, -320, 40),
                                    Point3D(2, -250, 40), Point3D(-200, -250, 40));
  World world;
  world.m_objects = { ground, border };
  PointLight light(RGBColor(0.4f, 0.4f, 0.4f), Point3D(0, 0, 300));
  ShadowMap shadow_map(&world, &light);
  shadow_map.render();

  ShadowMap::pcfShadows() = false;
  // Outside the shadow map image (far to the side but still in front of the
  // light): unoccluded.
  TASK_CHECK(approx(shadow_map.pointInShadow(Point3D(3000, 0, 0)), 0.0, 1e-6));
  // Behind the light's shadow camera (outside its near/far range): unoccluded.
  TASK_CHECK(approx(shadow_map.pointInShadow(Point3D(0, 0, 3000)), 0.0, 1e-6));

  // The centre tap of this point is shadowed and falls on the last row of the
  // map; its three downward PCF taps leave the image and count as unoccluded,
  // so the average is 6/9 rather than 9/9.
  const Point3D edge(0.5, -300.0, 0);
  TASK_CHECK(approx(shadow_map.pointInShadow(edge), 1.0, 1e-6));
  ShadowMap::pcfShadows() = true;
  TASK_CHECK(approx(shadow_map.pointInShadow(edge), 6.0 / 9.0, 1e-6));
  ShadowMap::pcfShadows() = false;
  return 0;
}

// ---------------------------------------------------------------- lifetime

int test_lifetime_shadow_maps_follow_lights() {
  FlatMaterial material(RGBColor(0.6f, 0.6f, 0.6f), RGBColor(0.1f, 0.1f, 0.1f), 1.0f);
  GeometryObject* ground = makeGround(&material, 400.0);
  World world;
  world.m_objects = { ground };
  ForwardProbe forward(&world);
  PerspectiveCamera camera(Point3D(0, -600, 400), Point3D(0, 0, 0), 120, 160, &forward, 2, 5000, 90.0f);
  world.m_camera = &camera;

  PointLight red(RGBColor(0.4f, 0.0f, 0.0f), Point3D(0, 0, 300));
  PointLight blue(RGBColor(0.0f, 0.0f, 0.3f), Point3D(600, 600, 300));

  world.m_lights = { &red };
  forward.render(false, true);
  TASK_CHECK(forward.shadowMapCount() == 1);

  // Repeated renders rebuild from the current light set instead of appending.
  forward.render(false, true);
  TASK_CHECK(forward.shadowMapCount() == 1);

  world.m_lights = { &red, &blue };
  forward.render(false, true);
  TASK_CHECK(forward.shadowMapCount() == 2);
  TASK_CHECK(forward.shadowMap(0)->getLight() == &red);
  TASK_CHECK(forward.shadowMap(1)->getLight() == &blue);

  // Reordering the lights rebuilds in the new order.
  world.m_lights = { &blue, &red };
  forward.render(false, true);
  TASK_CHECK(forward.shadowMapCount() == 2);
  TASK_CHECK(forward.shadowMap(0)->getLight() == &blue);
  TASK_CHECK(forward.shadowMap(1)->getLight() == &red);

  // Removing a light releases its map.
  world.m_lights = { &blue };
  forward.render(false, true);
  TASK_CHECK(forward.shadowMapCount() == 1);
  TASK_CHECK(forward.shadowMap(0)->getLight() == &blue);

  // Disabling shadows releases all maps; there must be no stale image.
  forward.render(false, false);
  TASK_CHECK(forward.shadowMapCount() == 0);

  // Re-enabling builds fresh maps.
  world.m_lights = { &red, &blue };
  forward.render(false, true);
  TASK_CHECK(forward.shadowMapCount() == 2);

  forward.render(false, false);
  TASK_CHECK(forward.shadowMapCount() == 0);
  return 0;
}

int test_lifetime_no_leaks() {
  // Baseline counts captured before the scene's own camera and rasterizer are
  // constructed, so live shadow cameras/rasterizers are easy to account for.
  const int cameras_before = Camera::liveInstances();
  const int rasterizers_before = ForwardRasterizer::liveInstances();

  FlatMaterial material(RGBColor(0.6f, 0.6f, 0.6f), RGBColor(0.1f, 0.1f, 0.1f), 1.0f);
  GeometryObject* ground = makeGround(&material, 400.0);
  World world;
  world.m_objects = { ground };
  ForwardProbe forward(&world);
  PerspectiveCamera camera(Point3D(0, -600, 400), Point3D(0, 0, 0), 120, 160, &forward, 2, 5000, 90.0f);
  world.m_camera = &camera;
  TASK_CHECK(Camera::liveInstances() == cameras_before + 1);
  TASK_CHECK(ForwardRasterizer::liveInstances() == rasterizers_before + 1);

  PointLight red(RGBColor(0.4f, 0.0f, 0.0f), Point3D(0, 0, 300));
  PointLight blue(RGBColor(0.0f, 0.0f, 0.3f), Point3D(600, 600, 300));

  // Each shadow map owns one camera and one rasterizer. Rendering repeatedly
  // with the same two lights must keep exactly one per light, not accumulate.
  for (int i = 0; i < 5; ++i) {
    world.m_lights = { &red, &blue };
    forward.render(false, true);
    TASK_CHECK(forward.shadowMapCount() == 2);
    TASK_CHECK(Camera::liveInstances() == cameras_before + 1 + 2);
    TASK_CHECK(ForwardRasterizer::liveInstances() == rasterizers_before + 1 + 2);
  }

  // Degenerate light set: empty list and switches must release everything.
  world.m_lights = {};
  forward.render(false, true);
  TASK_CHECK(forward.shadowMapCount() == 0);
  forward.render(false, false);
  world.m_lights = { &red };
  forward.render(false, true);
  TASK_CHECK(forward.shadowMapCount() == 1);
  TASK_CHECK(Camera::liveInstances() == cameras_before + 1 + 1);
  TASK_CHECK(ForwardRasterizer::liveInstances() == rasterizers_before + 1 + 1);
  forward.render(false, false);
  TASK_CHECK(forward.shadowMapCount() == 0);
  TASK_CHECK(Camera::liveInstances() == cameras_before + 1);
  TASK_CHECK(ForwardRasterizer::liveInstances() == rasterizers_before + 1);
  return 0;
}

int test_lifetime_forward_deferred_agree() {
  FlatMaterial material(RGBColor(0.6f, 0.6f, 0.6f), RGBColor(0.1f, 0.1f, 0.1f), 1.0f);
  GeometryObject* ground = makeGround(&material, 400.0);
  GeometryObject* blocker = makeQuad(&material,
                                     Point3D(-80, -80, 60), Point3D(80, -80, 60),
                                     Point3D(80, 80, 60), Point3D(-80, 80, 60));
  World world;
  world.m_objects = { ground, blocker };

  ForwardProbe forward(&world);
  DeferredProbe deferred(&world);
  PerspectiveCamera camera(Point3D(0, -600, 400), Point3D(0, 0, 0), 120, 160, &forward, 2, 5000, 90.0f);
  deferred.set_camera(&camera);
  world.m_camera = &camera;

  PointLight red(RGBColor(0.4f, 0.0f, 0.0f), Point3D(0, 0, 300));
  PointLight blue(RGBColor(0.0f, 0.0f, 0.3f), Point3D(600, 600, 300));
  world.m_lights = { &red, &blue };

  forward.render(true, true);
  deferred.render(true, true);

  // Same camera, geometry and material must produce the same visible surface
  // color on both paths, pixel for pixel.
  const int width = camera.get_width();
  const int height = camera.get_height();
  int mismatches = 0;
  for (int i = 0; i < width * height; ++i) {
    const RGBColor& a = forward.pixels()[i];
    const RGBColor& b = deferred.pixels()[i];
    if (!approx(a.r, b.r, 1e-6) || !approx(a.g, b.g, 1e-6) || !approx(a.b, b.b, 1e-6)) {
      ++mismatches;
    }
  }
  TASK_CHECK(mismatches == 0);

  // One light occluded, the other lit: the occluded light's direct term drops
  // but the other light and the ambient term survive.
  const Point3D occluded(0, 0, 0);
  const std::vector<double> factors = forward.factors(occluded);
  TASK_CHECK(factors.size() == 2);
  TASK_CHECK(approx(factors[0], 1.0, 1e-6));
  TASK_CHECK(approx(factors[1], 0.0, 1e-6));

  const int index = pixelIndexOf(forward, camera, occluded);
  TASK_CHECK(index >= 0);
  TASK_CHECK(approx(forward.pixels()[index].r, 0.1, 1e-5));  // red light blocked
  TASK_CHECK(approx(deferred.pixels()[index].r, 0.1, 1e-5));
  TASK_CHECK(forward.pixels()[index].b > 0.1 + 1e-4);         // blue light survives
  TASK_CHECK(approx(forward.pixels()[index].b, deferred.pixels()[index].b, 1e-6));

  // Blocking the red light must give exactly the same color as not having the
  // red light at all: a shadow only removes that light's own contribution.
  // Hand-computed for the blue light at (600,600,300), point (0,0,0), ground
  // normal +Z and camera at (0,-600,400):
  //   L = (2/3, 2/3, 1/3), NdotL = 1/3, V = (0, -0.832, 0.555)
  //   H = normalize(L + V) = (0.594, -0.144, 0.791), H dot V = 0.561
  //   diffuse = 0.6 * 1/3 = 0.2, specular = 0.1 * 0.561 = 0.0561
  //   b = 0.1 + 0.3 * (0.2 + 0.0561) = 0.1768
  world.m_lights = { &blue };
  forward.render(true, true);
  deferred.render(true, true);
  TASK_CHECK(approx(forward.pixels()[index].r, 0.1, 1e-5));
  TASK_CHECK(approx(forward.pixels()[index].b, deferred.pixels()[index].b, 1e-6));
  TASK_CHECK(approx(forward.pixels()[index].b, 0.1768, 1e-3));
  TASK_CHECK(approx(forward.pixels()[index].g, 0.1, 1e-5));

  // Removing the blocker lights the point with both lights again; the value is
  // the two-light formula applied to the real rendered geometry.
  world.m_objects = { ground };
  world.m_lights = { &red, &blue };
  forward.render(true, true);
  deferred.render(true, true);
  TASK_CHECK(approx(forward.pixels()[index].r, deferred.pixels()[index].r, 1e-6));
  TASK_CHECK(forward.pixels()[index].r > 0.9 * 0.375);
  return 0;
}

int test_lifetime_shadow_toggle_clears_stale_maps() {
  FlatMaterial material(RGBColor(0.6f, 0.6f, 0.6f), RGBColor(0.1f, 0.1f, 0.1f), 1.0f);
  GeometryObject* ground = makeGround(&material, 400.0);
  GeometryObject* blocker = makeQuad(&material,
                                     Point3D(-80, -80, 60), Point3D(80, -80, 60),
                                     Point3D(80, 80, 60), Point3D(-80, 80, 60));
  World world;
  world.m_objects = { ground, blocker };
  ForwardProbe forward(&world);
  PerspectiveCamera camera(Point3D(0, -600, 400), Point3D(0, 0, 0), 120, 160, &forward, 2, 5000, 90.0f);
  world.m_camera = &camera;
  PointLight red(RGBColor(0.4f, 0.0f, 0.0f), Point3D(0, 0, 300));
  PointLight blue(RGBColor(0.0f, 0.0f, 0.3f), Point3D(600, 600, 300));
  world.m_lights = { &red, &blue };

  const Point3D shadowed(0, 0, 0);
  const int index = pixelIndexOf(forward, camera, shadowed);
  TASK_CHECK(index >= 0);

  forward.render(true, true);
  TASK_CHECK(forward.shadowMapCount() == 2);
  const RGBColor shadowed_color = forward.pixels()[index];
  TASK_CHECK(approx(shadowed_color.r, 0.1, 1e-5));

  // Shading on, shadows off: the stale map must not be consulted.
  forward.render(true, false);
  TASK_CHECK(forward.shadowMapCount() == 0);
  TASK_CHECK(forward.pixels()[index].r > shadowed_color.r + 0.2);

  // The now-shadow-free color must match rendering with no blocker at all.
  world.m_objects = { ground };
  forward.render(true, true);
  const RGBColor unblocked = forward.pixels()[index];
  world.m_objects = { ground, blocker };
  forward.render(true, false);
  TASK_CHECK(approx(forward.pixels()[index].r, unblocked.r, 1e-6));
  TASK_CHECK(approx(forward.pixels()[index].g, unblocked.g, 1e-6));
  TASK_CHECK(approx(forward.pixels()[index].b, unblocked.b, 1e-6));

  // Shading itself off must not consult shadows either.
  forward.render(false, true);
  TASK_CHECK(forward.shadowMapCount() == 2);
  forward.render(false, false);
  TASK_CHECK(forward.shadowMapCount() == 0);
  return 0;
}

} // namespace

// ---------------------------------------------------------------- registry

extern const task_case task_cases[];
extern const size_t task_case_count;

const task_case task_cases[] = {
  { "lighting", "order_independence", test_lighting_order_independence },
  { "lighting", "per_light_accumulation", test_lighting_accumulates_per_light },
  { "lighting", "clamp_before_vertex_color", test_lighting_clamps_before_vertex_color },
  { "projection", "orthographic_linear_depth", test_projection_orthographic_linear_depth },
  { "projection", "perspective_inverse_depth", test_projection_perspective_inverse_depth },
  { "projection", "tilted_occluder", test_projection_tilted_occluder_shadow },
  { "projection", "edge_and_outside", test_projection_edge_and_outside_is_lit },
  { "lifetime", "shadow_maps_follow_lights", test_lifetime_shadow_maps_follow_lights },
  { "lifetime", "no_leaks", test_lifetime_no_leaks },
  { "lifetime", "forward_deferred_agree", test_lifetime_forward_deferred_agree },
  { "lifetime", "shadow_toggle_clears_maps", test_lifetime_shadow_toggle_clears_stale_maps },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
