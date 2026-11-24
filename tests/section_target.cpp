// Target tests for world-space single plane sectioning.
//
// The cases build analytic closed box shells (outer shells face outward,
// cavity shells are reversed) and check the section pass against half-space
// expectations independently of the final color image: cut position, solid
// versus cavity pixels (non-zero winding, not parity), axis-depth ownership,
// and cap material/normal routing through both render paths.

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "task_support/check.h"

#include "World.h"
#include "ForwardRasterizer.h"
#include "DeferredRasterizer.h"
#include "PERSPECTIVECAMERA.H"
#include "OrthographicCamera.h"
#include "Material.h"
#include "PointLight.h"
#include "SectionPass.h"

namespace {

constexpr uint32_t WIDTH = 24;
constexpr uint32_t HEIGHT = 24;
constexpr double EPS = 1e-6;

struct Shell {
  std::vector<Point3D> vertices;
  std::vector<uint32_t> indices;
};

// Analytic axis-aligned box, half extents (hx, hy, hz), centered at origin.
// outward == false reverses the winding (inward-facing cavity shell).
Shell makeBox(const double hx, const double hy, const double hz,
              const bool outward) {
  Shell shell;
  shell.vertices = {
    Point3D(-hx, -hy, -hz), Point3D(hx, -hy, -hz),
    Point3D(hx, hy, -hz), Point3D(-hx, hy, -hz),
    Point3D(-hx, -hy, hz), Point3D(hx, -hy, hz),
    Point3D(hx, hy, hz), Point3D(-hx, hy, hz)
  };
  const std::vector<uint32_t> outward_indices = {
    0, 3, 2, 0, 2, 1,       // -z
    4, 5, 6, 4, 6, 7,       // +z
    0, 1, 5, 0, 5, 4,       // -y
    3, 7, 6, 3, 6, 2,       // +y
    0, 4, 7, 0, 7, 3,       // -x
    1, 2, 6, 1, 6, 5        // +x
  };
  shell.indices = outward_indices;
  if (!outward) {
    for (size_t i = 0; i < outward_indices.size(); i += 3) {
      shell.indices[i + 1] = outward_indices[i + 2];
      shell.indices[i + 2] = outward_indices[i + 1];
    }
  }
  return shell;
}

Shell mergeShells(const Shell& first, const Shell& second) {
  Shell merged = first;
  const uint32_t base = static_cast<uint32_t>(first.vertices.size());
  for (const Point3D& vertex : second.vertices) {
    merged.vertices.push_back(vertex);
  }
  for (const uint32_t index : second.indices) {
    merged.indices.push_back(base + index);
  }
  return merged;
}

GeometryObject* makeObject(const Shell& shell, Material* material,
                           const Point3D& center) {
  const std::vector<RGBColor> colors(shell.vertices.size(),
                                     RGBColor(1.0f, 1.0f, 1.0f));
  return new GeometryObject(material, shell.vertices, colors,
                            std::vector<Vector2D>(), shell.indices, center);
}

bool closeTo(const double a, const double b, const double tolerance = 1e-5) {
  return std::fabs(a - b) <= tolerance;
}

uint32_t pixelIndex(const uint32_t x, const uint32_t y) {
  return y * WIDTH + x;
}

struct Fixture {
  World world;
  FlatMaterial material;
  PointLight light;
  std::vector<GeometryObject*> owned;

  Fixture()
    : material(RGBColor(0.4f, 0.4f, 0.4f), RGBColor(0.3f, 0.3f, 0.3f), 4.0f),
      light(RGBColor(1.0f, 1.0f, 1.0f), Point3D(-5, 6, -4)) {
    world.m_lights = {&light};
  }

  ~Fixture() {
    for (GeometryObject* object : owned) {
      delete object;
    }
  }

  GeometryObject* add(const Shell& shell, const Point3D& center) {
    GeometryObject* object = makeObject(shell, &material, center);
    owned.push_back(object);
    world.m_objects.push_back(object);
    return object;
  }
};

const RGBColor CAP_COLOR(0.9f, 0.4f, 0.2f);
const RGBColor CAP_DIFFUSE(0.7f, 0.3f, 0.15f);
const RGBColor CAP_SPECULAR(0.2f, 0.2f, 0.2f);

// A solid box centered at (0, 0, 10) with half size 2, cut at z = 9.
struct CutBox {
  Fixture fixture;
  PerspectiveCamera camera;
  ForwardRasterizer forward;
  DeferredRasterizer deferred;

  CutBox()
    : camera(Point3D(0, 0, 0), Point3D(0, 0, 1), HEIGHT, WIDTH, &forward,
             1, 100, 90.0f),
      forward(&fixture.world),
      deferred(&fixture.world) {
    fixture.world.m_camera = &camera;
    deferred.set_camera(&camera);
    fixture.add(makeBox(2.0, 2.0, 2.0, true), Point3D(0, 0, 10));
  }
};

// ----------------------------- winding group -----------------------------

int winding_solid_box() {
  CutBox cut;
  cut.forward.enableSection(Vector3D(0, 0, 1), -9.0, CAP_COLOR, CAP_DIFFUSE,
                            CAP_SPECULAR);
  cut.forward.render(true, false);

  const auto& mask = cut.forward.sectionCapMask();
  const auto& owner = cut.forward.sectionCapObject();
  const auto& caps = cut.forward.sectionCaps();
  const auto depth = cut.forward.get_depth_buffer();

  const uint32_t center = pixelIndex(WIDTH / 2, HEIGHT / 2);
  TASK_CHECK(mask[center] == 1);
  TASK_CHECK(owner[center] == 0);
  // The cap sits exactly on the plane z = 9 and wins the depth buffer.
  TASK_CHECK(closeTo(caps[center].position.z, 9.0));
  TASK_CHECK(closeTo(caps[center].position.x, 0.0, 1e-4));
  TASK_CHECK(closeTo(caps[center].position.y, 0.0, 1e-4));
  TASK_CHECK(closeTo(depth[center], caps[center].depth));
  // Cap normal points toward the retained region: -normalize(n).
  TASK_CHECK(closeTo(caps[center].normal.z, -1.0));
  TASK_CHECK(closeTo(caps[center].normal.x, 0.0));
  TASK_CHECK(closeTo(caps[center].normal.y, 0.0));

  // A corner ray misses the box, so there must be no cap there.
  bool corner_clean = true;
  for (uint32_t x : {0u, WIDTH - 1}) {
    for (uint32_t y : {0u, HEIGHT - 1}) {
      if (mask[pixelIndex(x, y)] != 0) {
        corner_clean = false;
      }
    }
  }
  TASK_CHECK(corner_clean);
  return 0;
}

int winding_nested_not_parity() {
  // Outer shell plus an inward cavity shell in ONE object. Winding must be
  // +1 in the solid ring and 0 in the cavity (parity would flip the ring to
  // odd and the cavity to odd as well, giving the opposite answer).
  Fixture fixture;
  ForwardRasterizer renderer(&fixture.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), HEIGHT, WIDTH,
                           &renderer, 1, 100, 90.0f);
  fixture.world.m_camera = &camera;

  const Shell nested =
    mergeShells(makeBox(3.0, 3.0, 3.0, true),
                makeBox(1.2, 1.2, 1.2, false));
  fixture.add(nested, Point3D(0, 0, 12));

  renderer.enableSection(Vector3D(0, 0, 1), -11.0, CAP_COLOR, CAP_DIFFUSE,
                         CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const uint32_t center = pixelIndex(WIDTH / 2, HEIGHT / 2);
  TASK_CHECK(mask[center] == 0);  // cavity: winding zero, must not be filled

  // A ray through the ring (offset but still inside the outer box).
  bool ring_found = false;
  for (uint32_t i = 0; i < mask.size(); ++i) {
    if (mask[i] == 1) {
      ring_found = true;
      TASK_CHECK(renderer.sectionCapObject()[i] == 0);
    }
  }
  TASK_CHECK(ring_found);
  return 0;
}

int winding_same_orientation_nested() {
  // Two same-orientation outward shells in one object: winding adds to 2 in
  // the inner region, which is still solid under the non-zero rule.
  Fixture fixture;
  ForwardRasterizer renderer(&fixture.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), HEIGHT, WIDTH,
                           &renderer, 1, 100, 90.0f);
  fixture.world.m_camera = &camera;

  const Shell doubled =
    mergeShells(makeBox(3.0, 3.0, 3.0, true),
                makeBox(1.2, 1.2, 1.2, true));
  fixture.add(doubled, Point3D(0, 0, 12));

  renderer.enableSection(Vector3D(0, 0, 1), -11.0, CAP_COLOR, CAP_DIFFUSE,
                         CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const uint32_t center = pixelIndex(WIDTH / 2, HEIGHT / 2);
  TASK_CHECK(mask[center] == 1);  // winding 2, non-zero => solid
  TASK_CHECK(renderer.sectionCapObject()[center] == 0);
  return 0;
}

int winding_shared_edge_diagonal() {
  // Replace the +z face triangulation with the opposite diagonal: pixels on
  // the shared diagonal must still be covered exactly once, so the winding
  // total never doubles or vanishes along it.
  Fixture fixture;
  ForwardRasterizer renderer(&fixture.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), HEIGHT, WIDTH,
                           &renderer, 1, 100, 90.0f);
  fixture.world.m_camera = &camera;

  Shell box = makeBox(2.0, 2.0, 2.0, true);
  // Replace the +z face triangulation (vertices 4,5,6,7) in place with the
  // opposite diagonal, keeping the shell closed. Original diagonal is 4-6
  // (4,5,6 / 4,6,7); the flipped one is 5-7 (4,5,7 / 5,6,7).
  box.indices[6] = 4;
  box.indices[7] = 5;
  box.indices[8] = 7;
  box.indices[9] = 5;
  box.indices[10] = 6;
  box.indices[11] = 7;
  fixture.add(box, Point3D(0, 0, 10));

  renderer.enableSection(Vector3D(0, 0, 1), -9.0, CAP_COLOR, CAP_DIFFUSE,
                         CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  uint32_t cap_count = 0;
  for (uint8_t value : mask) {
    TASK_CHECK(value == 0 || value == 1);
    cap_count += value;
  }
  TASK_CHECK(cap_count > 0);
  // The center pixel remains solid with exactly one owning object.
  const uint32_t center = pixelIndex(WIDTH / 2, HEIGHT / 2);
  TASK_CHECK(mask[center] == 1);
  TASK_CHECK(renderer.sectionCapObject()[center] == 0);
  return 0;
}

// --------------------------- projection group ----------------------------

int projection_oblique_perspective() {
  Fixture fixture;
  ForwardRasterizer renderer(&fixture.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), HEIGHT, WIDTH,
                           &renderer, 1, 100, 90.0f);
  fixture.world.m_camera = &camera;
  fixture.add(makeBox(2.5, 2.5, 2.5, true), Point3D(0, 0, 12));

  // Tilted plane n = normalize(0, 1, 2).
  const Vector3D normal(0.0, 1.0, 2.0);
  const double length = std::sqrt(5.0);
  const double d = -18.0;
  renderer.enableSection(normal, d, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  const auto& caps = renderer.sectionCaps();
  const auto& mask = renderer.sectionCapMask();
  const Vector3D expected_normal = normal * (-1.0 / length);
  bool checked = false;
  for (uint32_t i = 0; i < mask.size(); ++i) {
    if (mask[i] != 1) {
      continue;
    }
    checked = true;
    // Every cap point lies on the normalized plane.
    const Vector3D p(caps[i].position);
    const Vector3D n_norm = normal / length;
    TASK_CHECK(closeTo(n_norm * p + d / length, 0.0, 1e-5));
    TASK_CHECK(closeTo(caps[i].normal.x, expected_normal.x, 1e-6));
    TASK_CHECK(closeTo(caps[i].normal.y, expected_normal.y, 1e-6));
    TASK_CHECK(closeTo(caps[i].normal.z, expected_normal.z, 1e-6));
  }
  TASK_CHECK(checked);
  return 0;
}

int projection_oblique_orthographic() {
  Fixture fixture;
  ForwardRasterizer renderer(&fixture.world);
  OrthographicCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), HEIGHT, WIDTH,
                            &renderer, 1, 100);
  fixture.world.m_camera = &camera;
  fixture.add(makeBox(2.0, 2.0, 2.0, true), Point3D(0, 0, 10));

  renderer.enableSection(Vector3D(0.0, 1.0, 1.0), -9.5, CAP_COLOR,
                         CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const auto& caps = renderer.sectionCaps();
  const auto depth = renderer.get_depth_buffer();
  bool checked = false;
  for (uint32_t i = 0; i < mask.size(); ++i) {
    if (mask[i] != 1) {
      continue;
    }
    checked = true;
    // Orthographic rays travel along +z: the cut depth is the hit's z in
    // camera space and matches the shared depth buffer exactly.
    const Point3D hit_camera = camera.viewTransform(caps[i].position);
    TASK_CHECK(closeTo(hit_camera.z, caps[i].depth, 1e-6));
    TASK_CHECK(closeTo(depth[i], caps[i].depth, 1e-6));
    TASK_CHECK(caps[i].depth >= camera.get_near_plane() - EPS);
    TASK_CHECK(caps[i].depth <= camera.get_far_plane() + EPS);
  }
  TASK_CHECK(checked);
  return 0;
}

int projection_perspective_inverse_depth() {
  // Under perspective, cap depth is reconstructed with inverse axis-depth
  // interpolation consistency: unrasterizing the cap depth returns the exact
  // plane intersection point.
  CutBox cut;
  cut.forward.enableSection(Vector3D(0, 0, 1), -9.0, CAP_COLOR, CAP_DIFFUSE,
                            CAP_SPECULAR);
  cut.forward.render(true, false);

  const auto& caps = cut.forward.sectionCaps();
  const auto& mask = cut.forward.sectionCapMask();
  bool checked = false;
  for (uint32_t y = 0; y < HEIGHT; ++y) {
    for (uint32_t x = 0; x < WIDTH; ++x) {
      const uint32_t i = pixelIndex(x, y);
      if (mask[i] != 1) {
        continue;
      }
      checked = true;
      const Point3D rebuilt =
        cut.forward.unrasterize(Point2D(static_cast<double>(x),
                                        static_cast<double>(y)),
                                caps[i].depth);
      TASK_CHECK(closeTo(rebuilt.x, caps[i].position.x, 1e-5));
      TASK_CHECK(closeTo(rebuilt.y, caps[i].position.y, 1e-5));
      TASK_CHECK(closeTo(rebuilt.z, caps[i].position.z, 1e-5));
    }
  }
  TASK_CHECK(checked);
  return 0;
}

int projection_parallel_ray_no_cap() {
  // An orthographic camera looking along +z and a plane whose normal has no
  // z component: every ray is parallel, so no cap candidate may be produced.
  Fixture fixture;
  ForwardRasterizer renderer(&fixture.world);
  OrthographicCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), HEIGHT, WIDTH,
                            &renderer, 1, 100);
  fixture.world.m_camera = &camera;
  fixture.add(makeBox(2.0, 2.0, 2.0, true), Point3D(0, 0, 10));

  renderer.enableSection(Vector3D(1.0, 0.0, 0.0), 0.0, CAP_COLOR,
                         CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  for (uint8_t value : renderer.sectionCapMask()) {
    TASK_CHECK(value == 0);
  }
  return 0;
}

// -------------------------- composition group ----------------------------

int composition_both_paths_material_normal() {
  CutBox cut;
  // Tilt whose retained half-space removes the front shell over the box
  // center (n.p + d = y + z - 10), so the cap actually wins at the center.
  const Vector3D normal(0.0, 1.0, 1.0);
  const double d = -10.0;
  cut.forward.enableSection(normal, d, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  cut.deferred.enableSection(normal, d, CAP_COLOR, CAP_DIFFUSE, CAP_SPECULAR);
  cut.forward.render(true, false);
  cut.deferred.render(true, false);

  const uint32_t center = pixelIndex(WIDTH / 2, HEIGHT / 2);
  TASK_CHECK(cut.forward.sectionCapMask()[center] == 1);
  TASK_CHECK(cut.deferred.sectionCapMask()[center] == 1);

  // Deferred: the winning cap must be present in the shared GBuffer.
  const Vector3D expected_normal = normal * -1.0 / std::sqrt(2.0);
  const Vector3D gbuffer_normal(cut.deferred.normalBuffer()[center]);
  TASK_CHECK(closeTo(gbuffer_normal.x, expected_normal.x, 1e-5));
  TASK_CHECK(closeTo(gbuffer_normal.y, expected_normal.y, 1e-5));
  TASK_CHECK(closeTo(gbuffer_normal.z, expected_normal.z, 1e-5));
  TASK_CHECK(closeTo(cut.deferred.colorBuffer()[center].r, CAP_COLOR.r, 1e-5));
  TASK_CHECK(closeTo(cut.deferred.diffuseBuffer()[center].g,
                     CAP_DIFFUSE.g, 1e-5));
  TASK_CHECK(closeTo(cut.deferred.specularBuffer()[center].b,
                     CAP_SPECULAR.b, 1e-5));
  // Same depth and world position on both paths.
  TASK_CHECK(closeTo(cut.forward.sectionCaps()[center].depth,
                     cut.deferred.sectionCaps()[center].depth, 1e-6));
  TASK_CHECK(closeTo(cut.forward.sectionCaps()[center].position.x,
                     cut.deferred.sectionCaps()[center].position.x, 1e-5));
  return 0;
}

int composition_front_occluder() {
  // The plane function is monotone along a camera ray, so an occluder can
  // only hide caps on the pixels it actually covers. The small front box is
  // offset laterally; the tilted plane retains it while cutting the rear box
  // on pixels the front box does not cover.
  Fixture fixture;
  ForwardRasterizer renderer(&fixture.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), HEIGHT, WIDTH,
                           &renderer, 1, 100, 90.0f);
  fixture.world.m_camera = &camera;

  fixture.add(makeBox(2.0, 2.0, 2.0, true), Point3D(0, 0, 12));      // rear
  fixture.add(makeBox(0.5, 0.5, 0.5, true), Point3D(0, 2.5, 6));     // front

  // Plane 3y + z - 11 = 0. On the center ray (y = 0) it crosses the rear box
  // (z = 11, between faces 10 and 14) and clips its front shell, so the cap
  // wins. Around the front box (y ~ 2.5, z in [5.5, 6.5]) the plane value
  // stays positive, so that shell is retained and keeps its pixels.
  renderer.enableSection(Vector3D(0.0, 3.0, 1.0), -11.0, CAP_COLOR,
                         CAP_DIFFUSE, CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const auto& owner = renderer.sectionCapObject();
  const auto depth = renderer.get_depth_buffer();

  // The rear box cap wins on the unoccluded center pixel.
  const uint32_t center = pixelIndex(WIDTH / 2, HEIGHT / 2);
  TASK_CHECK(mask[center] == 1);
  TASK_CHECK(owner[center] == 0);
  TASK_CHECK(closeTo(depth[center], 11.0, 1e-5));

  // Pixels covered by the retained front box show a near surface and never a
  // cap: the cap cannot paint over the shell in front.
  bool occluder_seen = false;
  for (uint32_t i = 0; i < mask.size(); ++i) {
    if (depth[i] < 7.0) {
      occluder_seen = true;
      TASK_CHECK(mask[i] == 0);
      TASK_CHECK(owner[i] == SECTION_INVALID_OBJECT);
    }
  }
  TASK_CHECK(occluder_seen);
  return 0;
}

int composition_two_overlapping_objects_owner() {
  // Two partially overlapping solid objects: at equal depth the first object
  // in world order wins shared pixels, while each cap records its own object
  // index on the pixels only that object covers.
  Fixture fixture;
  ForwardRasterizer renderer(&fixture.world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), HEIGHT, WIDTH,
                           &renderer, 1, 100, 90.0f);
  fixture.world.m_camera = &camera;

  fixture.add(makeBox(2.0, 2.0, 2.0, true), Point3D(0, 0, 12));
  fixture.add(makeBox(1.5, 1.5, 1.5, true), Point3D(2.0, 0, 12));

  renderer.enableSection(Vector3D(0, 0, 1), -11.0, CAP_COLOR, CAP_DIFFUSE,
                         CAP_SPECULAR);
  renderer.render(true, false);

  const auto& owner = renderer.sectionCapObject();
  const auto& mask = renderer.sectionCapMask();
  const uint32_t center = pixelIndex(WIDTH / 2, HEIGHT / 2);
  TASK_CHECK(mask[center] == 1);
  // Equal depth on a shared pixel: the first object in world order wins.
  TASK_CHECK(owner[center] == 0);

  // Pixels covered only by the shifted second object belong to it.
  bool saw_second = false;
  for (uint32_t i = 0; i < owner.size(); ++i) {
    if (owner[i] == 1) {
      saw_second = true;
      TASK_CHECK(mask[i] == 1);
    }
  }
  TASK_CHECK(saw_second);
  return 0;
}

int composition_invalid_config_keeps_state() {
  CutBox cut;
  cut.forward.enableSection(Vector3D(0, 0, 1), -9.0, CAP_COLOR, CAP_DIFFUSE,
                            CAP_SPECULAR);
  cut.forward.render(true, false);
  const auto previous_depth = cut.forward.get_depth_buffer();
  TASK_CHECK(cut.forward.sectionEnabled());

  const SectionConfig before = cut.forward.sectionConfig();
  bool threw = false;
  try {
    cut.forward.enableSection(Vector3D(0, 0, 0), -9.0, CAP_COLOR, CAP_DIFFUSE,
                              CAP_SPECULAR);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(cut.forward.sectionEnabled());
  TASK_CHECK(closeTo(cut.forward.sectionConfig().d, before.d));
  TASK_CHECK(closeTo(cut.forward.sectionConfig().n.z, before.n.z));

  // Non-finite color is also rejected and the image is untouched.
  threw = false;
  try {
    cut.forward.updateSection(Vector3D(0, 0, 1), -9.0,
                              RGBColor(0.0f, 0.0f,
                                       std::numeric_limits<float>::quiet_NaN()),
                              CAP_DIFFUSE, CAP_SPECULAR);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  // Rendering still works after the rejected updates.
  cut.forward.render(true, false);
  const auto after_depth = cut.forward.get_depth_buffer();
  TASK_CHECK(previous_depth.size() == after_depth.size());
  for (size_t i = 0; i < after_depth.size(); ++i) {
    TASK_CHECK(closeTo(previous_depth[i], after_depth[i], 1e-9));
  }
  return 0;
}

int composition_plane_change_and_disable() {
  CutBox cut;
  cut.forward.enableSection(Vector3D(0, 0, 1), -9.0, CAP_COLOR, CAP_DIFFUSE,
                            CAP_SPECULAR);
  cut.forward.render(true, false);
  const auto first_mask = cut.forward.sectionCapMask();
  TASK_CHECK(!first_mask.empty());

  // Move the plane in front of the whole box: no caps remain.
  cut.forward.updateSection(Vector3D(0, 0, 1), -5.0, CAP_COLOR, CAP_DIFFUSE,
                            CAP_SPECULAR);
  TASK_CHECK(cut.forward.sectionEnabled());
  cut.forward.render(true, false);
  for (uint8_t value : cut.forward.sectionCapMask()) {
    TASK_CHECK(value == 0);
  }
  for (int32_t value : cut.forward.sectionCapObject()) {
    TASK_CHECK(value == SECTION_INVALID_OBJECT);
  }

  // Switch the plane back and interleave the two renderers: frame data is
  // rebuilt per renderer and per frame.
  cut.deferred.enableSection(Vector3D(0, 0, 1), -9.0, CAP_COLOR, CAP_DIFFUSE,
                             CAP_SPECULAR);
  cut.deferred.render(true, false);
  cut.forward.updateSection(Vector3D(0, 0, 1), -9.0, CAP_COLOR, CAP_DIFFUSE,
                            CAP_SPECULAR);
  cut.forward.render(true, false);
  TASK_CHECK(cut.forward.sectionCapMask()[pixelIndex(WIDTH / 2, HEIGHT / 2)]
              == 1);
  TASK_CHECK(cut.deferred.sectionCapMask()[pixelIndex(WIDTH / 2, HEIGHT / 2)]
              == 1);

  // Disabling clears the read-back and restores ordinary rendering: the
  // front face of the box (z = 8) is visible at the center again.
  cut.forward.disableSection();
  TASK_CHECK(!cut.forward.sectionEnabled());
  cut.forward.render(true, false);
  TASK_CHECK(cut.forward.sectionCapMask().empty());
  TASK_CHECK(cut.forward.sectionCapObject().empty());
  TASK_CHECK(cut.forward.sectionCaps().empty());
  const auto plain_depth = cut.forward.get_depth_buffer();
  TASK_CHECK(closeTo(plain_depth[pixelIndex(WIDTH / 2, HEIGHT / 2)], 8.0, 1e-5));
  return 0;
}

int composition_rejects_other_render_args() {
  CutBox cut;
  cut.forward.enableSection(Vector3D(0, 0, 1), -9.0, CAP_COLOR, CAP_DIFFUSE,
                            CAP_SPECULAR);
  bool threw = false;
  try {
    cut.forward.render(true, true);  // shadows not extended
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  threw = false;
  try {
    cut.forward.render(false, false);  // shading must stay enabled
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  // The rejected calls happened before any buffer allocation/modification:
  // a later valid render still works and produces the expected cap.
  cut.forward.render(true, false);
  TASK_CHECK(cut.forward.sectionCapMask()[pixelIndex(WIDTH / 2, HEIGHT / 2)]
              == 1);

  // Without sectioning, all parameter combinations remain available.
  cut.forward.disableSection();
  cut.forward.render(false, false);
  cut.forward.render(true, true);
  TASK_CHECK(cut.forward.sectionCapMask().empty());
  return 0;
}

int composition_object_transform_and_camera() {
  // Rotate and translate the object, and move the camera: caps must follow
  // the transformed geometry and keep correct ownership/depth.
  Fixture fixture;
  ForwardRasterizer renderer(&fixture.world);
  PerspectiveCamera camera(Point3D(-2, 1.5, 0), Point3D(0, 0, 1), HEIGHT,
                           WIDTH, &renderer, 1, 100, 90.0f);
  fixture.world.m_camera = &camera;

  GeometryObject* object =
      fixture.add(makeBox(2.0, 2.0, 2.0, true), Point3D(0, 0, 12));
  object->rotate_quat(35.0f, Vector3D(0, 1, 0));
  object->translate(Vector3D(0.5, -0.5, 0.0));

  renderer.enableSection(Vector3D(0, 0, 1), -11.0, CAP_COLOR, CAP_DIFFUSE,
                         CAP_SPECULAR);
  renderer.render(true, false);

  const auto& mask = renderer.sectionCapMask();
  const auto& caps = renderer.sectionCaps();
  bool checked = false;
  for (uint32_t i = 0; i < mask.size(); ++i) {
    if (mask[i] != 1) {
      continue;
    }
    checked = true;
    TASK_CHECK(renderer.sectionCapObject()[i] == 0);
    TASK_CHECK(closeTo(caps[i].position.z, 11.0, 1e-5));
    TASK_CHECK(caps[i].depth >= camera.get_near_plane() - EPS);
    TASK_CHECK(caps[i].depth <= camera.get_far_plane() + EPS);
  }
  TASK_CHECK(checked);
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
  {"winding", "solid_box", winding_solid_box},
  {"winding", "nested_cavity_not_parity", winding_nested_not_parity},
  {"winding", "same_orientation_nested", winding_same_orientation_nested},
  {"winding", "shared_edge_diagonal_flip", winding_shared_edge_diagonal},

  {"projection", "oblique_perspective", projection_oblique_perspective},
  {"projection", "oblique_orthographic", projection_oblique_orthographic},
  {"projection", "perspective_inverse_depth", projection_perspective_inverse_depth},
  {"projection", "parallel_ray_no_cap", projection_parallel_ray_no_cap},

  {"composition", "both_paths_material_normal", composition_both_paths_material_normal},
  {"composition", "front_occluder", composition_front_occluder},
  {"composition", "two_overlapping_objects_owner", composition_two_overlapping_objects_owner},
  {"composition", "invalid_config_keeps_state", composition_invalid_config_keeps_state},
  {"composition", "plane_change_and_disable", composition_plane_change_and_disable},
  {"composition", "rejects_other_render_args", composition_rejects_other_render_args},
  {"composition", "object_transform_and_camera", composition_object_transform_and_camera}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
