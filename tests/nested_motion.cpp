#include "task_support/check.h"
#include "minipbrt_scene.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

static bool near_double(double a, double b, double eps = 1e-7) {
  double scale = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
  return std::fabs(a - b) <= eps * scale;
}

static bool write_fixture(const char* filename, const char* contents) {
  std::string path = std::string(".task-build/") + filename;
  std::ofstream out(path.c_str());
  if (!out) {
    return false;
  }
  out << contents;
  return out.good();
}

static bool path_equals(const std::vector<uint32_t>& path,
                        uint32_t a = 0xffffffffu,
                        uint32_t b = 0xffffffffu,
                        uint32_t c = 0xffffffffu) {
  uint32_t expected[3] = { a, b, c };
  size_t count = 0;
  while (count < 3 && expected[count] != 0xffffffffu) {
    ++count;
  }
  if (path.size() != count) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (path[i] != expected[i]) {
      return false;
    }
  }
  return true;
}

static void transform_point(const double m[4][4], const float p[3], double q[3]) {
  for (int i = 0; i < 3; ++i) {
    q[i] = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3];
  }
}

static minipbrt::Transform identity_transform() {
  minipbrt::Transform t;
  for (int e = 0; e < 2; ++e) {
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        if (e == 0) t.start[i][j] = (i == j) ? 1.0f : 0.0f;
        else t.end[i][j] = (i == j) ? 1.0f : 0.0f;
      }
    }
  }
  return t;
}

static minipbrt::TriangleMesh* one_vertex_mesh() {
  minipbrt::TriangleMesh* mesh = new minipbrt::TriangleMesh();
  mesh->num_vertices = 1;
  mesh->P = new float[3];
  mesh->P[0] = 0.0f;
  mesh->P[1] = 0.0f;
  mesh->P[2] = 0.0f;
  mesh->num_indices = 3;
  mesh->indices = new int[3];
  mesh->indices[0] = mesh->indices[1] = mesh->indices[2] = 0;
  return mesh;
}

static void add_world_triangle(minipbrt::Scene& scene) {
  minipbrt::TriangleMesh* mesh = one_vertex_mesh();
  scene.shapes.push_back(mesh);
  scene.startTime = 0.0f;
  scene.endTime = 1.0f;
  mesh->shapeToWorld = identity_transform();
}

// q(u) = u * (2u - 1): internal minimum -1/8 at u=1/4, endpoint max 1.
// Root instance index 1 contributes 2u-1 in x; child instance index 0
// translates x by u.  The middle factor is singular at u=1/2.
static void build_quadratic_scene(minipbrt::Scene& scene) {
  scene.startTime = 0.0f;
  scene.endTime = 1.0f;
  scene.shapes.push_back(one_vertex_mesh());
  scene.shapes[0]->shapeToWorld = identity_transform();

  minipbrt::Object* leaf = new minipbrt::Object();
  leaf->firstShape = 0;
  leaf->numShapes = 1;
  minipbrt::Object* middle = new minipbrt::Object();
  scene.objects.push_back(leaf);
  scene.objects.push_back(middle);

  minipbrt::Instance* child = new minipbrt::Instance();
  child->ownerObject = 1;
  child->object = 0;
  child->instanceToWorld = identity_transform();
  child->instanceToWorld.end[0][3] = 1.0f; // tx = u

  minipbrt::Instance* root = new minipbrt::Instance();
  root->ownerObject = 0xffffffffu;
  root->object = 1;
  root->instanceToWorld = identity_transform();
  root->instanceToWorld.start[0][0] = -1.0f; // -1 + 2u
  root->instanceToWorld.end[0][0] = 1.0f;

  scene.instances.push_back(child); // global index 0
  scene.instances.push_back(root);  // global index 1
}

// q(u) = u(u - 1/2)(u - 1), with extrema +/- sqrt(3)/36.
static void build_cubic_scene(minipbrt::Scene& scene) {
  build_quadratic_scene(scene); // Recreate the same two-object/path layout.

  minipbrt::Instance* child = scene.instances[0];
  child->instanceToWorld = identity_transform();
  child->instanceToWorld.start[0][0] = -0.5f; // u - 1/2
  child->instanceToWorld.end[0][0] = 0.5f;

  minipbrt::Instance* root = scene.instances[1];
  root->instanceToWorld = identity_transform();
  root->instanceToWorld.start[0][0] = 0.0f; // u
  root->instanceToWorld.end[0][0] = 1.0f;

  minipbrt::Shape* shape = scene.shapes[0];
  shape->shapeToWorld = identity_transform();
  shape->shapeToWorld.start[0][3] = -1.0f; // tx = u - 1
  shape->shapeToWorld.end[0][3] = 0.0f;
}

static bool bounds_tight(double low, double high, double trueMin, double trueMax,
                         double epsilon, double trajectoryScale) {
  double s = std::max(1.0, trajectoryScale);
  double tolerance = epsilon + 1e-8 * s + 1e-12;
  return low <= trueMin &&
         high >= trueMax &&
         (trueMin - low) <= tolerance &&
         (high - trueMax) <= tolerance;
}

} // namespace

static int loading_nested_fixture(void) {
  static const char* includeSource =
    "ObjectBegin \"blob\"\n"
    "Shape \"trianglemesh\" \"integer indices\" [0 1 2]\n"
    "  \"point P\" [-1 -1 0  1 -1 0  0 1 0]\n"
    "ObjectEnd\n";

  static const char* mainSource =
    "TransformTimes 0 1\n"
    "WorldBegin\n"
    "Include \"nm_nested_include.pbrt\"\n"
    "Shape \"trianglemesh\" \"integer indices\" [0 1 2]\n"
    "  \"point P\" [0 0 0  1 0 0  0 1 0]\n"
    "ActiveTransform StartTime\n"
    "Translate 0 0 0\n"
    "ActiveTransform EndTime\n"
    "Translate 0 2 0\n"
    "ActiveTransform All\n"
    "ObjectInstance \"rig\"\n"
    "Identity\n"
    "ObjectBegin \"shared\"\n"
    "Shape \"trianglemesh\" \"integer indices\" [0 1 2]\n"
    "  \"point P\" [1 2 0  3 4 0  5 6 0]\n"
    "ObjectEnd\n"
    "ObjectBegin \"left\"\n"
    "Translate 2 0 0\n"
    "ObjectInstance \"shared\"\n"
    "ObjectEnd\n"
    "ObjectBegin \"right\"\n"
    "ActiveTransform StartTime\n"
    "Transform [-1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1]\n"
    "ActiveTransform EndTime\n"
    "Transform [-1 0 0 0  2 1 0 0  0 0 1 0  0 0 0 1]\n"
    "ActiveTransform All\n"
    "ReverseOrientation\n"
    "ObjectInstance \"shared\"\n"
    "ObjectEnd\n"
    "ObjectBegin \"rig\"\n"
    "Shape \"trianglemesh\" \"integer indices\" [0 1 2]\n"
    "  \"point P\" [7 8 0  9 8 0  8 10 0]\n"
    "ObjectInstance \"left\"\n"
    "ObjectInstance \"right\"\n"
    "ObjectEnd\n"
    "AttributeBegin\n"
    "Translate 7 8 9\n"
    "ReverseOrientation\n"
    "ObjectInstance \"blob\"\n"
    "AttributeEnd\n"
    "ObjectInstance \"blob\"\n"
    "WorldEnd\n";

  TASK_CHECK(write_fixture("nm_nested_include.pbrt", includeSource));
  TASK_CHECK(write_fixture("nm_nested_main.pbrt", mainSource));

  minipbrt::Loader loader;
  bool loaded = loader.load(".task-build/nm_nested_main.pbrt");
  if (!loaded && loader.error()) {
    std::fprintf(stderr, "loading fixture error %s:%lld: %s\n",
                 loader.error()->filename(), (long long)loader.error()->line(),
                 loader.error()->message());
  }
  TASK_CHECK(loaded);
  const minipbrt::Scene* scene = loader.borrow_scene();
  TASK_CHECK(scene != 0);
  TASK_CHECK(scene->shapes.size() == 4);
  TASK_CHECK(scene->objects.size() == 5);
  TASK_CHECK(scene->instances.size() == 7);

  TASK_CHECK(scene->objects[0]->name && std::strcmp(scene->objects[0]->name, "blob") == 0);
  TASK_CHECK(scene->objects[0]->firstShape == 0 && scene->objects[0]->numShapes == 1);
  TASK_CHECK(scene->objects[1]->firstShape == 2 && scene->objects[1]->numShapes == 1);
  TASK_CHECK(scene->objects[4]->firstShape == 3 && scene->objects[4]->numShapes == 1);

  TASK_CHECK(scene->instances[0]->ownerObject == 0xffffffffu);
  TASK_CHECK(scene->instances[0]->object == 4); // forward reference resolved
  TASK_CHECK(scene->instances[1]->ownerObject == 2);
  TASK_CHECK(scene->instances[2]->ownerObject == 3);
  TASK_CHECK(scene->instances[3]->ownerObject == 4);
  TASK_CHECK(scene->instances[4]->ownerObject == 4);
  TASK_CHECK(scene->instances[5]->ownerObject == 0xffffffffu);
  TASK_CHECK(scene->instances[6]->ownerObject == 0xffffffffu);

  // Original arrays and indices retain source identity.
  const minipbrt::TriangleMesh* shared =
    static_cast<const minipbrt::TriangleMesh*>(scene->shapes[2]);
  TASK_CHECK(shared->type() == minipbrt::ShapeType::TriangleMesh);
  TASK_CHECK(shared->num_vertices == 3 && shared->num_indices == 3);
  TASK_CHECK(shared->indices[0] == 0 && shared->indices[2] == 2);
  TASK_CHECK(shared->P[0] == 1.0f && shared->P[4] == 4.0f);

  // Root motion endpoint and AttributeBegin/End transform restoration.
  const minipbrt::Transform& rootMotion = scene->instances[0]->instanceToWorld;
  TASK_CHECK(near_double(rootMotion.start[1][3], 0.0));
  TASK_CHECK(near_double(rootMotion.end[1][3], 2.0));
  TASK_CHECK(near_double(scene->instances[5]->instanceToWorld.start[0][3], 7.0));
  TASK_CHECK(near_double(scene->instances[5]->instanceToWorld.start[2][3], 9.0));
  TASK_CHECK(near_double(scene->instances[6]->instanceToWorld.start[0][3], 0.0));
  TASK_CHECK(scene->instances[5]->reverseOrientation == true);
  TASK_CHECK(scene->instances[6]->reverseOrientation == false);

  return 0;
}

static int loading_undefined_and_duplicates(void) {
  TASK_CHECK(write_fixture("nm_undefined.pbrt",
    "WorldBegin\n"
    "ObjectInstance \"missing\"\n"
    "WorldEnd\n"));
  {
    minipbrt::Loader loader;
    TASK_CHECK(!loader.load(".task-build/nm_undefined.pbrt"));
    TASK_CHECK(loader.error() != 0);
    TASK_CHECK(loader.error()->line() == 2);
    TASK_CHECK(std::strstr(loader.error()->message(), "undefined object") != 0);
  }

  TASK_CHECK(write_fixture("nm_duplicate.pbrt",
    "WorldBegin\n"
    "ObjectBegin \"dup\"\n"
    "ObjectEnd\n"
    "ObjectBegin \"dup\"\n"
    "ObjectEnd\n"
    "WorldEnd\n"));
  {
    minipbrt::Loader loader;
    TASK_CHECK(!loader.load(".task-build/nm_duplicate.pbrt"));
    TASK_CHECK(loader.error() != 0);
    TASK_CHECK(loader.error()->line() == 4);
    TASK_CHECK(std::strstr(loader.error()->message(), "Duplicate") != 0);
  }
  return 0;
}

static int loading_cycles_and_nesting(void) {
  TASK_CHECK(write_fixture("nm_cycle_direct.pbrt",
    "WorldBegin\n"
    "ObjectBegin \"a\"\n"
    "ObjectInstance \"a\"\n"
    "ObjectEnd\n"
    "ObjectInstance \"a\"\n"
    "WorldEnd\n"));
  {
    minipbrt::Loader loader;
    TASK_CHECK(!loader.load(".task-build/nm_cycle_direct.pbrt"));
    TASK_CHECK(loader.error() != 0);
    TASK_CHECK(loader.error()->line() == 3);
    TASK_CHECK(std::strstr(loader.error()->message(), "yclic") != 0);
  }

  TASK_CHECK(write_fixture("nm_cycle_indirect.pbrt",
    "WorldBegin\n"
    "ObjectBegin \"a\"\n"
    "ObjectInstance \"b\"\n"
    "ObjectEnd\n"
    "ObjectBegin \"b\"\n"
    "ObjectInstance \"a\"\n"
    "ObjectEnd\n"
    "ObjectInstance \"a\"\n"
    "WorldEnd\n"));
  {
    minipbrt::Loader loader;
    TASK_CHECK(!loader.load(".task-build/nm_cycle_indirect.pbrt"));
    TASK_CHECK(loader.error() != 0);
    TASK_CHECK(loader.error()->line() == 6);
  }

  TASK_CHECK(write_fixture("nm_nested_begin.pbrt",
    "WorldBegin\n"
    "ObjectBegin \"outer\"\n"
    "ObjectBegin \"inner\"\n"
    "ObjectEnd\n"
    "ObjectEnd\n"
    "WorldEnd\n"));
  {
    minipbrt::Loader loader;
    TASK_CHECK(!loader.load(".task-build/nm_nested_begin.pbrt"));
    TASK_CHECK(loader.error() != 0);
  }
  return 0;
}

static int sampling_fixture_world_normals_flip(void) {
  minipbrt::Loader loader;
  TASK_CHECK(loader.load(".task-build/nm_nested_main.pbrt"));
  const minipbrt::Scene* scene = loader.borrow_scene();
  TASK_CHECK(scene != 0);

  std::vector<minipbrt::SceneDraw> draws;
  std::string error;
  bool ok = minipbrt::sample_scene(*scene, 0.5, draws, error);
  if (!ok) std::fprintf(stderr, "sample fixture: %s\n", error.c_str());
  TASK_CHECK(ok);
  TASK_CHECK(error.empty());
  TASK_CHECK(draws.size() == 6);

  TASK_CHECK(draws[0].shape == 1 && path_equals(draws[0].path));
  TASK_CHECK(draws[1].shape == 3 && path_equals(draws[1].path, 0));
  TASK_CHECK(draws[2].shape == 2 && path_equals(draws[2].path, 0, 3, 1));
  TASK_CHECK(draws[3].shape == 2 && path_equals(draws[3].path, 0, 4, 2));
  TASK_CHECK(scene->shapes[draws[2].shape] == scene->shapes[draws[3].shape]);
  TASK_CHECK(draws[4].shape == 0 && path_equals(draws[4].path, 5));
  TASK_CHECK(draws[5].shape == 0 && path_equals(draws[5].path, 6));

  const minipbrt::TriangleMesh* shared =
    static_cast<const minipbrt::TriangleMesh*>(scene->shapes[2]);
  float vertex[3] = { shared->P[0], shared->P[1], shared->P[2] };

  double leftWorld[3];
  transform_point(draws[2].world, vertex, leftWorld);
  TASK_CHECK(near_double(leftWorld[0], 3.0));
  TASK_CHECK(near_double(leftWorld[1], 3.0));
  TASK_CHECK(near_double(leftWorld[2], 0.0));

  double rightWorld[3];
  transform_point(draws[3].world, vertex, rightWorld);
  TASK_CHECK(near_double(rightWorld[0], 1.0));
  TASK_CHECK(near_double(rightWorld[1], 3.0));
  TASK_CHECK(near_double(rightWorld[2], 0.0));

  // Inverse transpose of [[-1,1,0],[0,1,0],[0,0,1]] at u=1/2.
  TASK_CHECK(near_double(draws[3].normal[0][0], -1.0));
  TASK_CHECK(near_double(draws[3].normal[0][1], 0.0));
  TASK_CHECK(near_double(draws[3].normal[1][0], 1.0));
  TASK_CHECK(near_double(draws[3].normal[1][1], 1.0));
  TASK_CHECK(near_double(draws[3].normal[2][2], 1.0));

  TASK_CHECK(draws[0].flipOrientation == false);
  TASK_CHECK(draws[1].flipOrientation == false);
  TASK_CHECK(draws[2].flipOrientation == false);
  TASK_CHECK(draws[3].flipOrientation == false); // instance XOR negative determinant
  TASK_CHECK(draws[4].flipOrientation == true);
  TASK_CHECK(draws[5].flipOrientation == false);

  // Element interpolation happens before multiplication: root ty is exactly 1.
  TASK_CHECK(near_double(draws[3].world[1][3], 1.0));
  return 0;
}

static int sampling_direct_validation_and_reuse(void) {
  minipbrt::Scene scene;
  add_world_triangle(scene);

  std::vector<minipbrt::SceneDraw> out;
  out.push_back(minipbrt::SceneDraw());
  out[0].shape = 1234;
  out[0].path.push_back(42);
  std::string error;

  TASK_CHECK(minipbrt::sample_scene(scene, 0.25, out, error));
  TASK_CHECK(out.size() == 1 && out[0].shape == 0 && out[0].path.empty());
  TASK_CHECK(near_double(out[0].world[0][0], 1.0));

  TASK_CHECK(minipbrt::sample_scene(scene, -5.0, out, error));
  TASK_CHECK(near_double(out[0].world[0][3], 0.0) && near_double(out[0].world[1][3], 0.0));
  TASK_CHECK(minipbrt::sample_scene(scene, 5.0, out, error));
  TASK_CHECK(near_double(out[0].world[0][0], 1.0));

  // Singular sampled world: failure preserves the previous output.
  minipbrt::Scene singular;
  add_world_triangle(singular);
  singular.shapes[0]->shapeToWorld.start[0][0] = 0.0f;
  singular.shapes[0]->shapeToWorld.start[1][1] = 0.0f;
  singular.shapes[0]->shapeToWorld.start[2][2] = 0.0f;
  singular.shapes[0]->shapeToWorld.end[0][0] = 0.0f;
  singular.shapes[0]->shapeToWorld.end[1][1] = 0.0f;
  singular.shapes[0]->shapeToWorld.end[2][2] = 0.0f;

  std::vector<minipbrt::SceneDraw> preserved;
  preserved.push_back(minipbrt::SceneDraw());
  preserved[0].shape = 777;
  preserved[0].path.push_back(9);
  bool ok = minipbrt::sample_scene(singular, 0.5, preserved, error);
  TASK_CHECK(!ok);
  TASK_CHECK(!error.empty());
  TASK_CHECK(preserved.size() == 1 && preserved[0].shape == 777);
  TASK_CHECK(preserved[0].path.size() == 1 && preserved[0].path[0] == 9);

  // Non-affine and non-finite caller-constructed matrices are rejected.
  minipbrt::Scene nonAffine;
  add_world_triangle(nonAffine);
  nonAffine.shapes[0]->shapeToWorld.start[3][0] = 0.25f;
  std::vector<minipbrt::SceneDraw> dummy;
  TASK_CHECK(!minipbrt::sample_scene(nonAffine, 0.0, dummy, error));

  minipbrt::Scene nonFinite;
  add_world_triangle(nonFinite);
  nonFinite.startTime = std::numeric_limits<float>::quiet_NaN();
  TASK_CHECK(!minipbrt::sample_scene(nonFinite, 0.0, dummy, error));

  // Invalid mesh index.
  minipbrt::Scene badMesh;
  add_world_triangle(badMesh);
  minipbrt::TriangleMesh* mesh = static_cast<minipbrt::TriangleMesh*>(badMesh.shapes[0]);
  mesh->indices[0] = 8;
  TASK_CHECK(!minipbrt::sample_scene(badMesh, 0.0, dummy, error));

  // Graph cycle.
  minipbrt::Scene cycle;
  cycle.objects.push_back(new minipbrt::Object());
  cycle.objects.push_back(new minipbrt::Object());
  minipbrt::Instance* c0 = new minipbrt::Instance();
  c0->ownerObject = 0; c0->object = 1; c0->instanceToWorld = identity_transform();
  minipbrt::Instance* c1 = new minipbrt::Instance();
  c1->ownerObject = 1; c1->object = 0; c1->instanceToWorld = identity_transform();
  cycle.instances.push_back(c0);
  cycle.instances.push_back(c1);
  TASK_CHECK(!minipbrt::sample_scene(cycle, 0.0, dummy, error));

  // Reachable non-triangle shape.
  minipbrt::Scene sphereScene;
  minipbrt::Sphere* sphere = new minipbrt::Sphere();
  sphere->shapeToWorld = identity_transform();
  sphereScene.shapes.push_back(sphere);
  TASK_CHECK(!minipbrt::sample_scene(sphereScene, 0.0, dummy, error));
  return 0;
}

static int bounds_quadratic_reference(void) {
  minipbrt::Scene scene;
  build_quadratic_scene(scene);

  std::vector<minipbrt::SceneMotionBounds> bounds;
  std::string error;
  bool ok = minipbrt::bound_scene_motion(scene, 0.0, 1.0, 1e-6, bounds, error);
  if (!ok) std::fprintf(stderr, "quadratic bounds: %s\n", error.c_str());
  TASK_CHECK(ok);
  TASK_CHECK(bounds.size() == 1);
  TASK_CHECK(bounds[0].shape == 0);
  TASK_CHECK(path_equals(bounds[0].path, 1, 0));
  TASK_CHECK(bounds_tight(bounds[0].min[0], bounds[0].max[0], -0.125, 1.0, 1e-6, 1.0));
  TASK_CHECK(bounds[0].min[1] <= 0.0 && bounds[0].max[1] >= 0.0);
  TASK_CHECK(bounds[0].max[1] - bounds[0].min[1] <= 1e-6 + 1e-8);

  // Interval [0.4, 0.6] straddles the singular u=1/2 matrix.
  std::vector<minipbrt::SceneMotionBounds> middle;
  ok = minipbrt::bound_scene_motion(scene, 0.4, 0.6, 1e-7, middle, error);
  TASK_CHECK(ok);
  TASK_CHECK(middle.size() == 1);
  TASK_CHECK(bounds_tight(middle[0].min[0], middle[0].max[0], -0.08, 0.12, 1e-7, 0.12));
  return 0;
}

static int bounds_cubic_reference(void) {
  minipbrt::Scene scene;
  build_cubic_scene(scene);

  const double rootValue = std::sqrt(3.0) / 36.0;
  std::vector<minipbrt::SceneMotionBounds> bounds;
  std::string error;
  bool ok = minipbrt::bound_scene_motion(scene, 0.0, 1.0, 1e-6, bounds, error);
  if (!ok) std::fprintf(stderr, "cubic bounds: %s\n", error.c_str());
  TASK_CHECK(ok);
  TASK_CHECK(bounds.size() == 1);
  TASK_CHECK(bounds_tight(bounds[0].min[0], bounds[0].max[0],
                          -rootValue, rootValue, 1e-6, rootValue));

  // A narrow epsilon must still satisfy the fixed relative allowance.
  std::vector<minipbrt::SceneMotionBounds> tight;
  TASK_CHECK(minipbrt::bound_scene_motion(scene, 0.0, 1.0, 1e-9, tight, error));
  TASK_CHECK(bounds_tight(tight[0].min[0], tight[0].max[0],
                          -rootValue, rootValue, 1e-9, rootValue));
  return 0;
}

static int bounds_instant_and_clamp(void) {
  minipbrt::Scene scene;
  build_quadratic_scene(scene);
  std::string error;

  std::vector<minipbrt::SceneMotionBounds> instant;
  TASK_CHECK(minipbrt::bound_scene_motion(scene, 0.25, 0.25, 1e-8, instant, error));
  TASK_CHECK(instant.size() == 1);
  TASK_CHECK(instant[0].min[0] <= -0.125 && instant[0].max[0] >= -0.125);
  TASK_CHECK((-0.125 - instant[0].min[0]) <= 1e-8 + 1e-8);
  TASK_CHECK((instant[0].max[0] + 0.125) <= 1e-8 + 1e-8);

  std::vector<minipbrt::SceneMotionBounds> before;
  TASK_CHECK(minipbrt::bound_scene_motion(scene, -2.0, -1.0, 1e-7, before, error));
  TASK_CHECK(before[0].min[0] <= 0.0 && before[0].max[0] >= 0.0);
  TASK_CHECK(before[0].max[0] - before[0].min[0] <= 2.1e-8);

  std::vector<minipbrt::SceneMotionBounds> after;
  TASK_CHECK(minipbrt::bound_scene_motion(scene, 2.0, 3.0, 1e-7, after, error));
  TASK_CHECK(after[0].min[0] <= 1.0 && after[0].max[0] >= 1.0);
  TASK_CHECK(after[0].max[0] - after[0].min[0] <= 2.1e-8);

  std::vector<minipbrt::SceneMotionBounds> partial;
  TASK_CHECK(minipbrt::bound_scene_motion(scene, -1.0, 0.25, 1e-7, partial, error));
  TASK_CHECK(bounds_tight(partial[0].min[0], partial[0].max[0], -0.125, 0.0, 1e-7, 0.125));
  return 0;
}

static int bounds_empty_and_failure_reuse(void) {
  minipbrt::Scene empty;
  empty.startTime = 0.0f;
  empty.endTime = 1.0f;
  empty.objects.push_back(new minipbrt::Object());
  empty.objects[0]->firstShape = 0xffffffffu;
  empty.objects[0]->numShapes = 0;

  std::vector<minipbrt::SceneMotionBounds> emptyOut;
  emptyOut.push_back(minipbrt::SceneMotionBounds());
  emptyOut[0].shape = 55;
  std::string error;
  TASK_CHECK(minipbrt::bound_scene_motion(empty, 0.0, 1.0, 1e-5, emptyOut, error));
  TASK_CHECK(error.empty());
  TASK_CHECK(emptyOut.empty());

  minipbrt::Scene bad;
  bad.startTime = 2.0f;
  bad.endTime = 1.0f; // startTime > endTime

  std::vector<minipbrt::SceneMotionBounds> preserved;
  preserved.push_back(minipbrt::SceneMotionBounds());
  preserved[0].shape = 999;
  preserved[0].path.push_back(3);
  preserved[0].min[0] = -4.0;
  preserved[0].max[0] = 7.0;

  bool ok = minipbrt::bound_scene_motion(bad, 0.0, 1.0, 1e-5, preserved, error);
  TASK_CHECK(!ok);
  TASK_CHECK(!error.empty());
  TASK_CHECK(preserved.size() == 1 && preserved[0].shape == 999);
  TASK_CHECK(preserved[0].path.size() == 1 && preserved[0].path[0] == 3);
  TASK_CHECK(preserved[0].min[0] == -4.0 && preserved[0].max[0] == 7.0);

  TASK_CHECK(!minipbrt::bound_scene_motion(bad, 0.0, 1.0, 0.0, preserved, error));

  // The same output object is reusable after a failed call.
  minipbrt::Scene quadratic;
  build_quadratic_scene(quadratic);
  TASK_CHECK(minipbrt::bound_scene_motion(quadratic, 0.0, 1.0, 1e-5, preserved, error));
  TASK_CHECK(preserved.size() == 1 && preserved[0].shape == 0);
  return 0;
}

const struct task_case task_cases[] = {
  { "loading", "nested_fixture", loading_nested_fixture },
  { "loading", "undefined_and_duplicates", loading_undefined_and_duplicates },
  { "loading", "cycles_and_nesting", loading_cycles_and_nesting },
  { "sampling", "fixture_world_normals_flip", sampling_fixture_world_normals_flip },
  { "sampling", "direct_validation_and_reuse", sampling_direct_validation_and_reuse },
  { "bounds", "quadratic_reference", bounds_quadratic_reference },
  { "bounds", "cubic_reference", bounds_cubic_reference },
  { "bounds", "instant_and_clamp", bounds_instant_and_clamp },
  { "bounds", "empty_and_failure_reuse", bounds_empty_and_failure_reuse }
};

const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
