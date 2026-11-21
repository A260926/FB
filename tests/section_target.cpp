// Section plane target tests. No main(): task_support/driver.c drives the
// registered cases. All solidity references are analytic half-space tests on
// axis aligned boxes and nested boxes; they never read back the final color.

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "World.h"
#include "ForwardRasterizer.h"
#include "DeferredRasterizer.h"
#include "PERSPECTIVECAMERA.H"
#include "OrthographicCamera.h"
#include "Material.h"
#include "DIRECTIONALLIGHT.H"
#include "SectionPass.h"

#include "task_support/check.h"

namespace {

constexpr double EPS = 1e-6;
constexpr int32_t INVALID = SectionPass::INVALID_OBJECT;

bool near(double a, double b, double eps = 1e-5) {
  return std::abs(a - b) <= eps;
}

struct BoxMesh {
  std::vector<Point3D> vertices;
  std::vector<uint32_t> indices;
};

// Axis aligned box centered at c with half extent h. Outward shells wind so
// the geometric normal points away from c; inward shells are reversed, which
// makes a cavity when nested inside an outward shell.
BoxMesh makeBox(const Point3D& c, double h, bool outward) {
  const Point3D p0(c.x - h, c.y - h, c.z - h);
  const Point3D p1(c.x + h, c.y - h, c.z - h);
  const Point3D p2(c.x + h, c.y + h, c.z - h);
  const Point3D p3(c.x - h, c.y + h, c.z - h);
  const Point3D p4(c.x - h, c.y - h, c.z + h);
  const Point3D p5(c.x + h, c.y - h, c.z + h);
  const Point3D p6(c.x + h, c.y + h, c.z + h);
  const Point3D p7(c.x - h, c.y + h, c.z + h);

  BoxMesh mesh;
  mesh.vertices = {p0, p1, p2, p3, p4, p5, p6, p7};

  const std::vector<uint32_t> outward_indices = {
    0, 3, 2, 0, 2, 1,       // z- face
    4, 5, 6, 4, 6, 7,       // z+ face
    0, 1, 5, 0, 5, 4,       // y- face
    3, 7, 6, 3, 6, 2,       // y+ face
    0, 4, 7, 0, 7, 3,       // x- face
    1, 2, 6, 1, 6, 5        // x+ face
  };

  mesh.indices.reserve(outward_indices.size());
  for (size_t i = 0; i < outward_indices.size(); i += 3) {
    if (outward) {
      mesh.indices.push_back(outward_indices[i]);
      mesh.indices.push_back(outward_indices[i + 1]);
      mesh.indices.push_back(outward_indices[i + 2]);
    } else {
      mesh.indices.push_back(outward_indices[i + 2]);
      mesh.indices.push_back(outward_indices[i + 1]);
      mesh.indices.push_back(outward_indices[i]);
    }
  }
  return mesh;
}

// Same box but the z- face uses the other diagonal, so every shared edge is
// still shared while two triangles cover the surface differently.
BoxMesh makeBoxFlippedDiagonal(const Point3D& c, double h) {
  BoxMesh mesh = makeBox(c, h, true);
  mesh.indices[0] = 0; mesh.indices[1] = 3; mesh.indices[2] = 1;
  mesh.indices[3] = 3; mesh.indices[4] = 2; mesh.indices[5] = 1;
  return mesh;
}

// Concatenates shell meshes into one shell collection: an outer outward shell
// and a reversed inner shell form a single hollow GeometryObject whose
// winding cancels to zero inside the cavity.
BoxMesh mergeMeshes(const std::vector<BoxMesh>& parts) {
  BoxMesh merged;
  uint32_t offset = 0;
  for (const BoxMesh& part : parts) {
    for (const Point3D& vertex : part.vertices) {
      merged.vertices.push_back(vertex);
    }
    for (uint32_t index : part.indices) {
      merged.indices.push_back(index + offset);
    }
    offset += static_cast<uint32_t>(part.vertices.size());
  }
  return merged;
}

BoxMesh makeHollowBox(const Point3D& c, double outer_h, double inner_h) {
  return mergeMeshes({makeBox(c, outer_h, true), makeBox(c, inner_h, false)});
}

BoxMesh makeNestedOutwardBox(const Point3D& c, double outer_h, double inner_h) {
  return mergeMeshes({makeBox(c, outer_h, true), makeBox(c, inner_h, true)});
}

GeometryObject* makeObject(Material* material, const BoxMesh& mesh,
                           const Point3D& model_center = Point3D(0, 0, 0)) {
  return new GeometryObject(material, mesh.vertices, std::vector<RGBColor>(),
                            std::vector<Vector2D>(), mesh.indices, model_center);
}

bool insideBox(const Point3D& p, const Point3D& c, double h) {
  return std::abs(p.x - c.x) <= h + EPS &&
         std::abs(p.y - c.y) <= h + EPS &&
         std::abs(p.z - c.z) <= h + EPS;
}

struct Ray {
  Point3D origin;
  Vector3D direction;
};

// Analytic ray/plane intersection (test side, independent of SectionPass).
bool rayPlane(const Ray& ray, const Vector3D& n, double d, Point3D& hit) {
  const double denom = n * ray.direction;
  if (std::abs(denom) <= 1e-12) return false;
  const double t = -(n * Vector3D(ray.origin) + d) / denom;
  hit = ray.origin + ray.direction * t;
  return std::isfinite(t);
}

// Integer pixel ray for a perspective camera at the origin looking +z with
// fov 90 and unit aspect (identity orientation).
Ray perspectivePixelRay(int px, int py, int width, int height) {
  const double ndc_x = -1.0 + 2.0 * px / width;
  const double ndc_y = -1.0 + 2.0 * py / height;
  return {Point3D(0, 0, 0), Vector3D(ndc_x, ndc_y, 1.0)};
}

// Integer pixel ray for an orthographic camera looking +z.
Ray orthographicPixelRay(const Point3D& camera_pos, int px, int py,
                         int width, int height) {
  const double ndc_x = -1.0 + 2.0 * px / width;
  const double ndc_y = -1.0 + 2.0 * py / height;
  return {Point3D(camera_pos.x + ndc_x * width / 2.0,
                  camera_pos.y + ndc_y * height / 2.0,
                  camera_pos.z),
          Vector3D(0.0, 0.0, 1.0)};
}

// Analytic nonzero winding for a stack of shells around one center.
// orientations[i] is +1 for an outward shell, -1 for a reversed cavity shell.
int analyticWinding(const Point3D& p, const Point3D& center,
                    const std::vector<double>& halves,
                    const std::vector<int>& orientations) {
  int winding = 0;
  for (size_t i = 0; i < halves.size(); ++i) {
    if (insideBox(p, center, halves[i])) winding += orientations[i];
  }
  return winding;
}

const RGBColor CAP_COLOR(0.2f, 0.7f, 0.3f);
const RGBColor CAP_DIFFUSE(0.3f, 0.6f, 0.2f);
const RGBColor CAP_SPECULAR(0.8f, 0.8f, 0.1f);

struct Scene {
  World world;
  std::vector<Light*> lights;
  FlatMaterial material;
  FlatMaterial occluder_material;

  Scene()
    : world(),
      lights({new DirectionalLight(RGBColor(1.0f, 1.0f, 1.0f),
                                   Vector3D(0.3, 0.5, -1.0))}),
      material(RGBColor(0.4f, 0.4f, 0.5f), RGBColor(0.2f), 4.0f),
      occluder_material(RGBColor(0.9f, 0.1f, 0.1f), RGBColor(0.1f), 2.0f) {
    world.m_lights = lights;
  }

  ~Scene() {
    for (GeometryObject* object : world.m_objects) delete object;
    // Light's destructor is not virtual: do not delete through the base.
  }
};

struct ForwardProbe : ForwardRasterizer {
  using ForwardRasterizer::ForwardRasterizer;
  const std::vector<RGBColor>& pixels() const { return m_pixels; }
};

struct DeferredProbe : DeferredRasterizer {
  using DeferredRasterizer::DeferredRasterizer;
  const std::vector<RGBColor>& pixels() const { return m_pixels; }
};

int countCaps(const std::vector<unsigned char>& mask) {
  int count = 0;
  for (unsigned char v : mask) count += v;
  return count;
}

bool samePixels(const std::vector<RGBColor>& a, const std::vector<RGBColor>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!near(a[i].r, b[i].r, 0.0) || !near(a[i].g, b[i].g, 0.0) ||
        !near(a[i].b, b[i].b, 0.0)) {
      return false;
    }
  }
  return true;
}

// ------------------------------ winding group ------------------------------

int solidBox() {
  Scene scene;
  ForwardProbe renderer(&scene.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 32, 32,
                           &renderer, 1, 100, 90.0f);
  scene.world.m_camera = &camera;

  const Point3D center(0, 0, 15);
  GeometryObject* box = makeObject(&scene.material, makeBox(center, 4.0, true));
  scene.world.m_objects = {box};

  renderer.enableSection(Vector3D(0, 0, 1), -14.0,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const auto& owner = renderer.sectionCapObject();
  const auto depth = renderer.get_depth_buffer();
  TASK_CHECK(mask.size() == 32 * 32);
  TASK_CHECK(countCaps(mask) > 0);

  const Vector3D n(0, 0, 1);
  for (int py = 0; py < 32; ++py) {
    for (int px = 0; px < 32; ++px) {
      const uint32_t i = py * 32 + px;
      const Ray ray = perspectivePixelRay(px, py, 32, 32);
      Point3D hit;
      const bool hit_plane = rayPlane(ray, n, -14.0, hit);
      const bool solid = hit_plane && insideBox(hit, center, 4.0);
      TASK_CHECK(mask[i] == (solid ? 1 : 0));
      if (solid) {
        TASK_CHECK(owner[i] == 0);
        TASK_CHECK(near(n * Vector3D(hit) - 14.0, 0.0));
        TASK_CHECK(near(depth[i], hit.z, 1e-4));
        TASK_CHECK(depth[i] >= camera.get_near_plane());
        TASK_CHECK(depth[i] <= camera.get_far_plane());
      } else {
        TASK_CHECK(owner[i] == INVALID);
      }
    }
  }
  return 0;
}

int nestedSameDirection() {
  Scene scene;
  ForwardProbe renderer(&scene.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 32, 32,
                           &renderer, 1, 100, 90.0f);
  scene.world.m_camera = &camera;

  const Point3D center(0, 0, 15);
  // One object: two same-direction outward shells (winding 2 at the core).
  GeometryObject* nested =
    makeObject(&scene.material, makeNestedOutwardBox(center, 4.0, 2.0));
  scene.world.m_objects = {nested};

  renderer.enableSection(Vector3D(0, 0, 1), -14.0,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const auto& owner = renderer.sectionCapObject();

  int analytic_caps = 0;
  int inner_region = 0;
  for (int py = 0; py < 32; ++py) {
    for (int px = 0; px < 32; ++px) {
      const uint32_t i = py * 32 + px;
      Point3D hit;
      const bool hit_plane =
        rayPlane(perspectivePixelRay(px, py, 32, 32), Vector3D(0, 0, 1), -14.0, hit);
      const int winding = analyticWinding(hit, center, {4.0, 2.0}, {+1, +1});
      const bool solid = hit_plane && winding != 0;
      if (solid) ++analytic_caps;
      if (hit_plane && insideBox(hit, center, 2.0)) ++inner_region;
      TASK_CHECK(mask[i] == (solid ? 1 : 0));
      if (solid) TASK_CHECK(owner[i] == 0);
    }
  }
  // Same-direction nesting gives winding 2, never a parity-style hole.
  TASK_CHECK(inner_region > 0);
  TASK_CHECK(analytic_caps == countCaps(mask));
  return 0;
}

int nestedReversedCavity() {
  Scene scene;
  ForwardProbe renderer(&scene.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 32, 32,
                           &renderer, 1, 100, 90.0f);
  scene.world.m_camera = &camera;

  const Point3D center(0, 0, 15);
  // One hollow object: outward outer shell plus a reversed inner cavity shell.
  GeometryObject* hollow =
    makeObject(&scene.material, makeHollowBox(center, 4.0, 2.0));
  scene.world.m_objects = {hollow};

  renderer.enableSection(Vector3D(0, 0, 1), -14.0,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  int cavity_pixels = 0;
  int shell_pixels = 0;
  for (int py = 0; py < 32; ++py) {
    for (int px = 0; px < 32; ++px) {
      const uint32_t i = py * 32 + px;
      Point3D hit;
      const bool hit_plane =
        rayPlane(perspectivePixelRay(px, py, 32, 32), Vector3D(0, 0, 1), -14.0, hit);
      const int winding = analyticWinding(hit, center, {4.0, 2.0}, {+1, -1});
      const bool solid = hit_plane && winding != 0;
      TASK_CHECK(mask[i] == (solid ? 1 : 0));
      if (hit_plane && insideBox(hit, center, 2.0)) {
        ++cavity_pixels;
        TASK_CHECK(mask[i] == 0); // cavity must not be filled
      }
      if (solid) ++shell_pixels;
    }
  }
  TASK_CHECK(cavity_pixels > 0);
  TASK_CHECK(shell_pixels > 0);
  return 0;
}

int invalidConfiguration() {
  Scene scene;
  ForwardProbe renderer(&scene.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 24, 24,
                           &renderer, 1, 100, 90.0f);
  scene.world.m_camera = &camera;
  GeometryObject* box = makeObject(&scene.material,
                                   makeBox(Point3D(0, 0, 15), 4.0, true));
  scene.world.m_objects = {box};

  // update() while disabled is rejected.
  bool threw = false;
  try { renderer.updateSection(Vector3D(0, 0, 1), -14.0,
                               CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR); }
  catch (const std::logic_error&) { threw = true; }
  TASK_CHECK(threw);
  TASK_CHECK(!renderer.sectionEnabled());

  renderer.enableSection(Vector3D(0, 0, 1), -14.0,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);

  auto reject = [&](const Vector3D& n, double d,
                    const RGBColor& c, const RGBColor& df, const RGBColor& sp) {
    bool bad = false;
    try { renderer.enableSection(n, d, c, df, sp); }
    catch (const std::invalid_argument&) { bad = true; }
    return bad;
  };

  const RGBColor inf_color(std::numeric_limits<float>::infinity(), 1.0f, 1.0f);
  TASK_CHECK(reject(Vector3D(0, 0, 0), -14.0, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR));
  TASK_CHECK(reject(Vector3D(0, 0, 1),
                    std::numeric_limits<double>::quiet_NaN(),
                    CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR));
  TASK_CHECK(reject(Vector3D(0, 0, 1), -14.0, inf_color, CAP_DIFFUSE, CAP_SPECULAR));
  TASK_CHECK(reject(Vector3D(0, 0, 1), -14.0, CAP_COLOR,
                    RGBColor(1.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f),
                    CAP_SPECULAR));

  // Failed updates keep the old plane and the old image.
  renderer.render(true, false);
  const std::vector<RGBColor> before = renderer.pixels();
  TASK_CHECK(countCaps(renderer.sectionCapMask()) > 0);

  bool rejected = false;
  try { renderer.render(true, true); }
  catch (const std::invalid_argument&) { rejected = true; }
  TASK_CHECK(rejected);
  rejected = false;
  try { renderer.render(false, false); }
  catch (const std::invalid_argument&) { rejected = true; }
  TASK_CHECK(rejected);
  TASK_CHECK(samePixels(renderer.pixels(), before));
  TASK_CHECK(renderer.sectionEnabled());

  // Normal rendering after the rejected calls still produces caps.
  renderer.render(true, false);
  TASK_CHECK(countCaps(renderer.sectionCapMask()) > 0);
  return 0;
}

// ---------------------------- projection group -----------------------------

int tiltedPerspective() {
  Scene scene;
  ForwardProbe renderer(&scene.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 32, 32,
                           &renderer, 1, 100, 90.0f);
  scene.world.m_camera = &camera;

  const Point3D center(0, 0, 15);
  GeometryObject* box = makeObject(&scene.material, makeBox(center, 4.0, true));
  scene.world.m_objects = {box};

  const Vector3D n_raw(0.3, 0.0, 1.0);
  Vector3D n = n_raw;
  n.normalize();
  const double d = -14.5 / std::sqrt(1.0 + 0.3 * 0.3);
  renderer.enableSection(n_raw, -14.5, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const auto depth = renderer.get_depth_buffer();
  int caps = 0;
  for (int py = 0; py < 32; ++py) {
    for (int px = 0; px < 32; ++px) {
      const uint32_t i = py * 32 + px;
      Point3D hit;
      const bool hit_plane =
        rayPlane(perspectivePixelRay(px, py, 32, 32), n, d, hit);
      const bool solid = hit_plane && insideBox(hit, center, 4.0);
      TASK_CHECK(mask[i] == (solid ? 1 : 0));
      if (solid) {
        ++caps;
        TASK_CHECK(near(n * Vector3D(hit) + d, 0.0, 1e-4));
        TASK_CHECK(near(depth[i], hit.z, 1e-4));
      }
    }
  }
  TASK_CHECK(caps > 0);
  return 0;
}

int tiltedOrthographic() {
  Scene scene;
  ForwardProbe renderer(&scene.world);
  const Point3D cam_pos(0, -6, 0);
  OrthographicCamera camera(cam_pos, Point3D(0, -6, 10), 32, 32,
                            &renderer, 1, 100);
  scene.world.m_camera = &camera;

  // Half-integer center keeps every integer-pixel cut point off the box
  // faces and edges, matching the production input guarantee.
  const Point3D center(0.5, 0.5, 15);
  GeometryObject* box = makeObject(&scene.material, makeBox(center, 4.0, true));
  scene.world.m_objects = {box};

  const Vector3D n(0, 0, 1);
  renderer.enableSection(n, -14.0, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const auto depth = renderer.get_depth_buffer();
  int caps = 0;
  for (int py = 0; py < 32; ++py) {
    for (int px = 0; px < 32; ++px) {
      const uint32_t i = py * 32 + px;
      Point3D hit;
      const Ray ray = orthographicPixelRay(cam_pos, px, py, 32, 32);
      const bool hit_plane = rayPlane(ray, n, -14.0, hit);
      const bool solid = hit_plane && insideBox(hit, center, 4.0);
      TASK_CHECK(mask[i] == (solid ? 1 : 0));
      if (solid) {
        ++caps;
        // Orthographic axis depth is linear: exactly the plane depth.
        TASK_CHECK(near(depth[i], 14.0, 1e-4));
        TASK_CHECK(near(hit.z, 14.0, 1e-4));
      }
    }
  }
  TASK_CHECK(caps > 0);
  return 0;
}

int sharedEdgeDiagonal() {
  Scene scene;
  ForwardProbe renderer_a(&scene.world);
  ForwardProbe renderer_b(&scene.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 32, 32,
                           &renderer_a, 1, 100, 90.0f);
  renderer_b.set_camera(&camera);
  scene.world.m_camera = &camera;

  GeometryObject* box_a = makeObject(&scene.material,
                                     makeBox(Point3D(0, 0, 15), 4.0, true));
  GeometryObject* box_b = makeObject(&scene.material,
                                     makeBoxFlippedDiagonal(Point3D(0, 0, 15), 4.0));
  const size_t triangle_count = box_a->triangles().size();
  TASK_CHECK(triangle_count == box_b->triangles().size());

  scene.world.m_objects = {box_a};
  renderer_a.enableSection(Vector3D(0, 0, 1), -14.0,
                           CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer_a.render(true, false);

  scene.world.m_objects = {box_b};
  renderer_b.enableSection(Vector3D(0, 0, 1), -14.0,
                           CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer_b.render(true, false);

  TASK_CHECK(renderer_a.sectionCapMask() == renderer_b.sectionCapMask());
  TASK_CHECK(renderer_a.sectionCapObject() == renderer_b.sectionCapObject());
  TASK_CHECK(countCaps(renderer_a.sectionCapMask()) > 0);
  // Meshes stay untouched by the section pass.
  TASK_CHECK(box_a->triangles().size() == triangle_count);
  TASK_CHECK(box_b->triangles().size() == triangle_count);
  return 0;
}

int parallelAndRangeExclusion() {
  Scene scene;
  ForwardProbe renderer(&scene.world);
  const Point3D cam_pos(0, -6, 0);
  OrthographicCamera camera(cam_pos, Point3D(0, -6, 10), 32, 32,
                            &renderer, 1, 100);
  scene.world.m_camera = &camera;

  GeometryObject* box = makeObject(&scene.material,
                                   makeBox(Point3D(0.5, 0.5, 15), 4.0, true));
  scene.world.m_objects = {box};

  // Horizontal plane: its normal is orthogonal to the orthographic +z view
  // axis, so every pixel ray is parallel and there is no cap candidate.
  renderer.enableSection(Vector3D(0, 1, 0), -0.5,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);
  TASK_CHECK(countCaps(renderer.sectionCapMask()) == 0);
  for (unsigned char v : renderer.sectionCapMask()) TASK_CHECK(v == 0);
  for (int32_t owner : renderer.sectionCapObject()) TASK_CHECK(owner == INVALID);

  // Plane behind the far plane: intersections fall outside near/far and must
  // not become cap candidates either.
  renderer.updateSection(Vector3D(0, 0, 1), -5000.0,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);
  TASK_CHECK(countCaps(renderer.sectionCapMask()) == 0);

  // A direct ray/plane query reports the same exclusions.
  SectionPlane parallel = makeSectionPlane(Vector3D(0, 1, 0), -0.5,
                                           CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  Point3D hit;
  double axis_depth;
  TASK_CHECK(!sectionRayPlane(parallel, camera, Point2D(16, 16), hit, axis_depth));
  SectionPlane beyond = makeSectionPlane(Vector3D(0, 0, 1), -5000.0,
                                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  TASK_CHECK(!sectionRayPlane(beyond, camera, Point2D(16, 16), hit, axis_depth));

  // A valid plane on the same setup produces caps again.
  renderer.updateSection(Vector3D(0, 0, 1), -14.0,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);
  TASK_CHECK(countCaps(renderer.sectionCapMask()) > 0);
  return 0;
}

// ---------------------------- composition group ----------------------------

int frontOcclusion() {
  Scene scene;
  ForwardProbe renderer(&scene.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 32, 32,
                           &renderer, 1, 100, 90.0f);
  scene.world.m_camera = &camera;

  GeometryObject* box = makeObject(&scene.material,
                                   makeBox(Point3D(0, 0, 15), 4.0, true));
  // Retained-side occluder, fully in front of the z = 14 cut.
  GeometryObject* occluder = makeObject(&scene.occluder_material,
                                        makeBox(Point3D(0, 0, 12.5), 1.0, true));
  scene.world.m_objects = {occluder, box};

  // Normal -z keeps the near side (z <= 14). A retained closed front shell is
  // always in front of the cap, so the depth competition must let the shell
  // win and never let a cap bleed through it.
  renderer.enableSection(Vector3D(0, 0, -1), 14.0,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const auto& owner = renderer.sectionCapObject();
  const auto depth = renderer.get_depth_buffer();

  // Central ray hits the retained occluder first: no cap may overwrite it.
  const uint32_t center = 16 * 32 + 16;
  TASK_CHECK(mask[center] == 0);
  TASK_CHECK(owner[center] == INVALID);
  TASK_CHECK(depth[center] < 14.0 - EPS);

  // Global occlusion invariant:
  //  * any pixel with a retained surface in front of the cut carries no cap;
  //  * every winning cap sits exactly on the plane axis depth and cannot pass
  //    through a closer retained shell or object.
  bool retained_front = false;
  for (uint32_t i = 0; i < mask.size(); ++i) {
    if (depth[i] < 14.0 - EPS) {
      TASK_CHECK(mask[i] == 0);
      TASK_CHECK(owner[i] == INVALID);
      retained_front = true;
    }
    if (mask[i]) {
      TASK_CHECK(near(depth[i], 14.0, 1e-4));
    }
  }
  TASK_CHECK(retained_front);

  // The same scene keeping the far side shows the cap, proving the occluder
  // only suppresses it where a retained surface genuinely wins the depth.
  ForwardProbe far_renderer(&scene.world);
  far_renderer.set_camera(&camera);
  far_renderer.enableSection(Vector3D(0, 0, 1), -14.0,
                             CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  far_renderer.render(true, false);
  TASK_CHECK(countCaps(far_renderer.sectionCapMask()) > 0);
  return 0;
}

int overlappingObjectsOwnership() {
  Scene scene;
  ForwardProbe renderer(&scene.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 32, 32,
                           &renderer, 1, 100, 90.0f);
  scene.world.m_camera = &camera;

  GeometryObject* box_a = makeObject(&scene.material,
                                     makeBox(Point3D(-1.5, 0, 15), 3.0, true));
  GeometryObject* box_b = makeObject(&scene.occluder_material,
                                     makeBox(Point3D(1.5, 0, 15), 3.0, true));
  scene.world.m_objects = {box_a, box_b};

  renderer.enableSection(Vector3D(0, 0, 1), -14.0,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const auto& owner = renderer.sectionCapObject();

  int only_a = 0, only_b = 0, overlap = 0;
  for (int py = 0; py < 32; ++py) {
    for (int px = 0; px < 32; ++px) {
      const uint32_t i = py * 32 + px;
      Point3D hit;
      const bool ok = rayPlane(perspectivePixelRay(px, py, 32, 32),
                               Vector3D(0, 0, 1), -14.0, hit);
      const bool in_a = ok && insideBox(hit, Point3D(-1.5, 0, 15), 3.0);
      const bool in_b = ok && insideBox(hit, Point3D(1.5, 0, 15), 3.0);
      if (in_a && in_b) {
        ++overlap;
        TASK_CHECK(mask[i] == 1);
        TASK_CHECK(owner[i] == 0); // m_objects order wins equal-depth caps
      } else if (in_a) {
        ++only_a;
        TASK_CHECK(owner[i] == 0);
      } else if (in_b) {
        ++only_b;
        TASK_CHECK(owner[i] == 1);
      } else {
        TASK_CHECK(mask[i] == 0);
        TASK_CHECK(owner[i] == INVALID);
      }
    }
  }
  TASK_CHECK(only_a > 0 && only_b > 0 && overlap > 0);
  return 0;
}

int capMaterialAndNormalBothPaths() {
  Scene scene;
  ForwardProbe forward(&scene.world);
  DeferredProbe deferred(&scene.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 32, 32,
                           &forward, 1, 100, 90.0f);
  deferred.set_camera(&camera);
  scene.world.m_camera = &camera;

  GeometryObject* box = makeObject(&scene.material,
                                   makeBox(Point3D(0, 0, 15), 4.0, true));
  scene.world.m_objects = {box};

  const Vector3D n_raw(0.0, 0.5, 1.0);
  const double length = std::sqrt(1.0 + 0.25);
  const Vector3D cap_normal(-0.0 / length, -0.5 / length, -1.0 / length);
  Vector3D n_unit = n_raw;
  n_unit.normalize();
  const double d_normalized = -14.0 / length;

  forward.enableSection(n_raw, -14.0, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  deferred.enableSection(n_raw, -14.0, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  forward.render(true, false);
  deferred.render(true, false);

  TASK_CHECK(forward.sectionCapMask() == deferred.sectionCapMask());
  TASK_CHECK(forward.sectionCapObject() == deferred.sectionCapObject());
  // Stored configuration is normalized together with d.
  TASK_CHECK(near(forward.sectionPlaneNormal().x, n_unit.x, 1e-9));
  TASK_CHECK(near(forward.sectionPlaneD(), d_normalized, 1e-9));

  const auto& fmask = forward.sectionCapMask();
  const auto& dmask = deferred.sectionCapMask();
  int checked = 0;
  for (uint32_t i = 0; i < fmask.size(); ++i) {
    if (!fmask[i] || !dmask[i]) continue;
    ++checked;
    const int px = i % 32;
    const int py = i / 32;

    // The cap position is the true ray/plane intersection on both paths.
    Point3D analytic_hit;
    TASK_CHECK(rayPlane(perspectivePixelRay(px, py, 32, 32),
                        n_unit, d_normalized, analytic_hit));
    const Point3D fpos =
      forward.unrasterize(Point2D(px, py), forward.get_depth_buffer()[i]);
    const Point3D dpos =
      deferred.unrasterize(Point2D(px, py), deferred.get_depth_buffer()[i]);
    TASK_CHECK(near(fpos.x, analytic_hit.x, 1e-4));
    TASK_CHECK(near(fpos.y, analytic_hit.y, 1e-4));
    TASK_CHECK(near(fpos.z, analytic_hit.z, 1e-4));
    TASK_CHECK(near(dpos.x, analytic_hit.x, 1e-4));
    TASK_CHECK(near(dpos.y, analytic_hit.y, 1e-4));
    TASK_CHECK(near(dpos.z, analytic_hit.z, 1e-4));

    // Deferred: the winning cap is stored in the shared GBuffer, not painted
    // over afterwards.
    const Vector3D stored_normal(deferred.normalBuffer()[i]);
    TASK_CHECK(near(stored_normal.x, cap_normal.x, 1e-4));
    TASK_CHECK(near(stored_normal.y, cap_normal.y, 1e-4));
    TASK_CHECK(near(stored_normal.z, cap_normal.z, 1e-4));
    TASK_CHECK(near(deferred.diffuseBuffer()[i].r, CAP_DIFFUSE.r, 1e-5));
    TASK_CHECK(near(deferred.specularBuffer()[i].g, CAP_SPECULAR.g, 1e-5));
    TASK_CHECK(near(deferred.colorBuffer()[i].b, CAP_COLOR.b, 1e-5));

    // Forward: the cap fragment went through the unchanged Material::shade.
    const Point3D position = fpos;
    const Fragment expected(position, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR,
                            cap_normal);
    const RGBColor expected_color =
      Material::shade(scene.world.m_lights, camera, expected);
    const RGBColor actual = forward.pixels()[i];
    TASK_CHECK(near(actual.r, expected_color.r, 1e-4));
    TASK_CHECK(near(actual.g, expected_color.g, 1e-4));
    TASK_CHECK(near(actual.b, expected_color.b, 1e-4));
  }
  TASK_CHECK(checked > 0);
  return 0;
}

int changePlaneAndDisable() {
  Scene scene;
  ForwardProbe renderer(&scene.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), 32, 32,
                           &renderer, 1, 100, 90.0f);
  scene.world.m_camera = &camera;

  GeometryObject* box = makeObject(&scene.material,
                                   makeBox(Point3D(0, 0, 15), 4.0, true));
  scene.world.m_objects = {box};
  const size_t triangles_before = box->triangles().size();

  renderer.enableSection(Vector3D(0, 0, 1), -14.0,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);
  const int first_caps = countCaps(renderer.sectionCapMask());
  TASK_CHECK(first_caps > 0);

  // Move the cut deeper: the per-frame data must be rebuilt.
  renderer.updateSection(Vector3D(0, 0, 1), -17.0,
                         CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);
  const int second_caps = countCaps(renderer.sectionCapMask());
  TASK_CHECK(second_caps > 0);
  TASK_CHECK(second_caps != first_caps);
  const auto depth = renderer.get_depth_buffer();
  for (uint32_t i = 0; i < renderer.sectionCapMask().size(); ++i) {
    if (renderer.sectionCapMask()[i]) TASK_CHECK(near(depth[i], 17.0, 1e-4));
  }

  // Disabling clears the readback and restores ordinary rendering.
  renderer.disableSection();
  TASK_CHECK(!renderer.sectionEnabled());
  TASK_CHECK(renderer.sectionCapMask().empty());
  TASK_CHECK(renderer.sectionCapObject().empty());
  renderer.render(true, false);
  TASK_CHECK(renderer.sectionCapMask().empty());
  TASK_CHECK(renderer.sectionCapObject().empty());

  // Geometry, material and World are untouched.
  TASK_CHECK(box->triangles().size() == triangles_before);
  TASK_CHECK(scene.world.m_objects.size() == 1);
  TASK_CHECK(scene.world.m_objects[0] == box);
  return 0;
}

int interleavedRenderersAndTransforms() {
  Scene scene;
  ForwardProbe forward(&scene.world);
  DeferredProbe deferred(&scene.world);
  const Point3D cam_pos(0, -6, 0);
  OrthographicCamera camera(cam_pos, Point3D(0, -6, 10), 32, 32,
                            &forward, 1, 100);
  deferred.set_camera(&camera);
  scene.world.m_camera = &camera;

  // One hollow object translated as a whole: translation preserves the
  // analytic half spaces of both shells. Half offsets keep integer-pixel cut
  // points off the shell surfaces (production input guarantee).
  GeometryObject* hollow = makeObject(&scene.material,
                                      makeHollowBox(Point3D(0, 0, 0), 4.0, 2.0));
  hollow->translate(Vector3D(2.5, 0.5, 15));
  scene.world.m_objects = {hollow};

  const Vector3D n(0, 0, 1);
  forward.enableSection(n, -14.0, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  deferred.enableSection(n, -14.0, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);

  // Interleave the two renderers; each rebuilds its own frame data.
  forward.render(true, false);
  deferred.render(true, false);
  forward.render(true, false);

  TASK_CHECK(forward.sectionCapMask() == deferred.sectionCapMask());
  TASK_CHECK(forward.sectionCapObject() == deferred.sectionCapObject());

  const Point3D center(2.5, 0.5, 15);
  const auto& mask = forward.sectionCapMask();
  int cavity_pixels = 0, shell_pixels = 0;
  for (int py = 0; py < 32; ++py) {
    for (int px = 0; px < 32; ++px) {
      const uint32_t i = py * 32 + px;
      Point3D hit;
      const Ray ray = orthographicPixelRay(cam_pos, px, py, 32, 32);
      const bool ok = rayPlane(ray, n, -14.0, hit);
      const int winding = analyticWinding(hit, center, {4.0, 2.0}, {+1, -1});
      const bool solid = ok && winding != 0;
      TASK_CHECK(mask[i] == (solid ? 1 : 0));
      if (ok && insideBox(hit, center, 2.0)) {
        ++cavity_pixels;
        TASK_CHECK(mask[i] == 0);
      }
      if (solid) ++shell_pixels;
    }
  }
  TASK_CHECK(cavity_pixels > 0 && shell_pixels > 0);

  // A rotated hollow box keeps its cavity (rotation is a rigid transform).
  Scene rotated_scene;
  ForwardProbe rotated(&rotated_scene.world);
  OrthographicCamera rotated_camera(cam_pos, Point3D(0, -6, 10), 32, 32,
                                    &rotated, 1, 100);
  rotated_scene.world.m_camera = &rotated_camera;
  GeometryObject* solid = makeObject(&rotated_scene.material,
                                     makeBox(Point3D(0, 0, 15), 4.0, true));
  GeometryObject* rotated_hollow = makeObject(&rotated_scene.material,
                                      makeHollowBox(Point3D(8, 0, 15), 4.0, 2.0));
  rotated_hollow->rotate_quat(35.0f, Vector3D(0, 1, 0));
  rotated_scene.world.m_objects = {solid, rotated_hollow};
  rotated.enableSection(n, -14.0, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  rotated.render(true, false);
  TASK_CHECK(countCaps(rotated.sectionCapMask()) > 0);
  return 0;
}

} // namespace

const struct task_case task_cases[] = {
  {"winding", "solid_box", solidBox},
  {"winding", "nested_same_direction", nestedSameDirection},
  {"winding", "nested_reversed_cavity", nestedReversedCavity},
  {"winding", "invalid_configuration", invalidConfiguration},

  {"projection", "tilted_perspective", tiltedPerspective},
  {"projection", "tilted_orthographic", tiltedOrthographic},
  {"projection", "shared_edge_diagonal", sharedEdgeDiagonal},
  {"projection", "parallel_and_range_exclusion", parallelAndRangeExclusion},

  {"composition", "front_occlusion", frontOcclusion},
  {"composition", "overlapping_objects_ownership", overlappingObjectsOwnership},
  {"composition", "cap_material_normal_both_paths", capMaterialAndNormalBothPaths},
  {"composition", "change_plane_and_disable", changePlaneAndDisable},
  {"composition", "interleaved_renderers_transforms", interleavedRenderersAndTransforms},
};

const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
