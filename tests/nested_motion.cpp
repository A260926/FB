// Tests for nested object instancing, shutter-time sampling and continuous
// motion bounds. This file is compiled together with task_support/driver.c
// (which supplies main) and registers its cases in the three required groups.

#include "minipbrt_scene.h"
#include "task_support/check.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace {

using namespace minipbrt;

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

const char* kFixtureDir = "tests/fixtures";

std::string fixture_path(const char* name)
{
  std::string path = kFixtureDir;
  path += "/";
  path += name;
  return path;
}

// Generated fixtures live in a temporary directory so running the tests does
// not dirty the source tree.
std::string temp_dir()
{
  const char* tmp = std::getenv("TMPDIR");
  if (tmp == nullptr || tmp[0] == '\0') {
    tmp = ".";
  }
  std::string dir = tmp;
  if (!dir.empty() && dir[dir.size() - 1] != '/') {
    dir += '/';
  }
  dir += "minipbrt_nested_motion_test";
  return dir;
}

std::string temp_path(const char* name)
{
  std::string path = temp_dir();
  path += "/";
  path += name;
  return path;
}

void mkdir_tmp()
{
#ifdef _WIN32
  _mkdir(temp_dir().c_str());
#else
  mkdir(temp_dir().c_str(), 0777);
#endif
}

bool write_temp(const char* name, const char* contents, std::string& path)
{
  path = temp_path(name);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  size_t len = std::strlen(contents);
  bool ok = std::fwrite(contents, 1, len, f) == len;
  std::fclose(f);
  return ok;
}

void set_identity(Transform& transform)
{
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      transform.start[r][c] = (r == c) ? 1.0f : 0.0f;
      transform.end[r][c] = (r == c) ? 1.0f : 0.0f;
    }
  }
}

void set_matrix(Transform& transform, const double m[4][4])
{
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      transform.start[r][c] = float(m[r][c]);
      transform.end[r][c] = float(m[r][c]);
    }
  }
}

bool close_to(double a, double b, double tolerance)
{
  return std::fabs(a - b) <= tolerance;
}


// ---------------------------------------------------------------------------
// Hand-built scene construction (exercises the validation that does not
// depend on the loader).
// ---------------------------------------------------------------------------

struct BuiltScene {
  Scene scene;
  TriangleMesh* mesh;
  Object* leaf;
  Object* branch;
  Instance* rootInstance;
  Instance* childInstance;
};

// A world-direct shape plus a two-level animated hierarchy:
//   world shape (identity)
//   root instance -> branch (animated rotate) -> leaf (shear) -> mesh
void build_hierarchy(BuiltScene& built)
{
  Scene& scene = built.scene;
  scene.startTime = 0.0f;
  scene.endTime = 1.0f;

  const double worldVertices[9] = { 0.0, 0.0, 0.0,  2.0, 0.0, 0.0,  0.0, 2.0, 0.0 };
  TriangleMesh* worldMesh = new TriangleMesh();
  worldMesh->num_vertices = 3;
  worldMesh->P = new float[9];
  for (int i = 0; i < 9; i++) worldMesh->P[i] = float(worldVertices[i]);
  worldMesh->num_indices = 3;
  worldMesh->indices = new int[3];
  worldMesh->indices[0] = 0;
  worldMesh->indices[1] = 1;
  worldMesh->indices[2] = 2;
  scene.shapes.push_back(worldMesh);
  set_identity(worldMesh->shapeToWorld);

  TriangleMesh* mesh = new TriangleMesh();
  mesh->num_vertices = 3;
  mesh->P = new float[9];
  for (int i = 0; i < 9; i++) mesh->P[i] = float(worldVertices[i]);
  mesh->num_indices = 3;
  mesh->indices = new int[3];
  mesh->indices[0] = 0;
  mesh->indices[1] = 1;
  mesh->indices[2] = 2;
  scene.shapes.push_back(mesh);

  Object* leaf = new Object();
  leaf->name = new char[5];
  std::strcpy(const_cast<char*>(leaf->name), "leaf");
  leaf->firstShape = 1;
  leaf->numShapes = 1;
  set_identity(leaf->objectToInstance);
  scene.objects.push_back(leaf);
  mesh->object = 0;

  Object* branch = new Object();
  branch->name = new char[7];
  std::strcpy(const_cast<char*>(branch->name), "branch");
  branch->firstShape = kInvalidIndex;
  branch->numShapes = 0;
  set_identity(branch->objectToInstance);
  scene.objects.push_back(branch);

  // Root instance: translate from (0,0,0) to (2,0,0) over the shutter.
  Instance* root = new Instance();
  root->object = 1;
  root->ownerObject = kInvalidIndex;
  set_identity(root->instanceToWorld);
  root->instanceToWorld.start[0][3] = 0.0f;
  root->instanceToWorld.end[0][3] = 2.0f;
  scene.instances.push_back(root);

  // Child instance: a rotation that is not a linear interpolation of the two
  // endpoint matrices, so interpolation order matters.
  Instance* child = new Instance();
  child->object = 0;
  child->ownerObject = 1;
  set_identity(child->instanceToWorld);
  child->instanceToWorld.start[0][0] = 1.0f; child->instanceToWorld.start[0][1] = 0.0f;
  child->instanceToWorld.start[1][0] = 0.0f; child->instanceToWorld.start[1][1] = 1.0f;
  child->instanceToWorld.end[0][0] = 0.0f;   child->instanceToWorld.end[0][1] = -1.0f;
  child->instanceToWorld.end[1][0] = 1.0f;   child->instanceToWorld.end[1][1] = 0.0f;
  scene.instances.push_back(child);

  // Shape transform: shear that is also animated.
  double shearStart[4][4] = {
    { 1.0, 0.0, 0.0, 0.0 },
    { 0.0, 1.0, 0.0, 0.0 },
    { 0.0, 0.0, 1.0, 0.0 },
    { 0.0, 0.0, 0.0, 1.0 },
  };
  double shearEnd[4][4] = {
    { 1.0, 0.5, 0.0, 0.0 },
    { 0.0, 1.0, 0.0, 0.0 },
    { 0.0, 0.0, 1.0, 0.0 },
    { 0.0, 0.0, 0.0, 1.0 },
  };
  set_matrix(mesh->shapeToWorld, shearStart);
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      mesh->shapeToWorld.end[r][c] = float(shearEnd[r][c]);

  built.mesh = mesh;
  built.leaf = leaf;
  built.branch = branch;
  built.rootInstance = root;
  built.childInstance = child;
}


// Reference implementation of the chain multiplication at a given weight,
// using only the public data. Returns shape 1's world matrix (the leaf mesh).
void reference_world(const Scene& scene, double u, double out[4][4])
{
  double root[4][4];
  double child[4][4];
  double shape[4][4];
  const Transform& rootT = scene.instances[0]->instanceToWorld;
  const Transform& childT = scene.instances[1]->instanceToWorld;
  const Transform& shapeT = scene.shapes[1]->shapeToWorld;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      root[r][c] = (1.0 - u) * rootT.start[r][c] + u * rootT.end[r][c];
      child[r][c] = (1.0 - u) * childT.start[r][c] + u * childT.end[r][c];
      shape[r][c] = (1.0 - u) * shapeT.start[r][c] + u * shapeT.end[r][c];
    }
  }
  double tmp[4][4];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      double s = 0.0;
      for (int k = 0; k < 4; k++) s += root[r][k] * child[k][c];
      tmp[r][c] = s;
    }
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      double s = 0.0;
      for (int k = 0; k < 4; k++) s += tmp[r][k] * shape[k][c];
      out[r][c] = s;
    }
}

// Transform the leaf mesh's three vertices by a matrix.
void transform_vertices(const TriangleMesh* mesh, const double m[4][4], double out[9])
{
  for (unsigned int v = 0; v < 3; v++) {
    for (int r = 0; r < 3; r++) {
      out[v * 3 + r] = m[r][0] * mesh->P[v * 3 + 0] +
                       m[r][1] * mesh->P[v * 3 + 1] +
                       m[r][2] * mesh->P[v * 3 + 2] + m[r][3];
    }
  }
}

void invert3(const double m[3][3], double out[3][3])
{
  double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
  out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
  out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
  out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
  out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
  out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
  out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
  out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
  out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
}


// ---------------------------------------------------------------------------
// Loading cases
// ---------------------------------------------------------------------------

int loading_forward_diamond()
{
  Loader loader;
  std::string path = fixture_path("nested_scene.pbrt");
  TASK_CHECK(loader.load(path.c_str()));

  Scene* scene = loader.borrow_scene();
  TASK_CHECK(scene != nullptr);
  TASK_CHECK(scene->shapes.size() == 3);
  TASK_CHECK(scene->objects.size() == 4);
  TASK_CHECK(scene->instances.size() == 5);

  // "mid" and "leaf" are defined after they are referenced by "outerA".
  TASK_CHECK(scene->objects[0]->name != nullptr &&
             std::strcmp(scene->objects[0]->name, "mid") == 0);
  TASK_CHECK(scene->objects[1]->name != nullptr &&
             std::strcmp(scene->objects[1]->name, "leaf") == 0);

  // Object shape ranges describe direct shapes only; instances are separate.
  TASK_CHECK(scene->objects[0]->firstShape == 1);
  TASK_CHECK(scene->objects[0]->numShapes == 1);
  TASK_CHECK(scene->objects[1]->firstShape == 2);
  TASK_CHECK(scene->objects[1]->numShapes == 1);
  TASK_CHECK(scene->objects[2]->numShapes == 0);
  TASK_CHECK(scene->objects[3]->numShapes == 0);
  TASK_CHECK(scene->shapes[0]->object == kInvalidIndex);
  TASK_CHECK(scene->shapes[1]->object == 0);
  TASK_CHECK(scene->shapes[2]->object == 1);

  // Instance ownership.
  TASK_CHECK(scene->instances[0]->object == 2 && scene->instances[0]->ownerObject == kInvalidIndex);
  TASK_CHECK(scene->instances[1]->object == 3 && scene->instances[1]->ownerObject == kInvalidIndex);
  TASK_CHECK(scene->instances[2]->object == 1 && scene->instances[2]->ownerObject == 0);
  TASK_CHECK(scene->instances[3]->object == 0 && scene->instances[3]->ownerObject == 2);
  TASK_CHECK(scene->instances[4]->object == 0 && scene->instances[4]->ownerObject == 3);

  // Shared geometry is not copied: 3 shapes, but shape 1 is used by four paths.
  TASK_CHECK(scene->shapes.size() == 3);
  TASK_CHECK(std::strcmp(scene->objects[1]->name, "leaf") == 0);

  // AttributeBegin/End inside outerA must restore the transform: the
  // "Translate 100 0 0" is scoped to the block, so the instance pointing at
  // "mid" starts at outerA's origin and only the ActiveTransform animation
  // (0 -> 4 along x) is kept.
  TASK_CHECK(close_to(scene->instances[3]->instanceToWorld.start[0][3], 0.0, 1e-6));
  TASK_CHECK(close_to(scene->instances[3]->instanceToWorld.end[0][3], 4.0, 1e-6));
  TASK_CHECK(close_to(scene->instances[3]->instanceToWorld.start[0][0], 1.0, 1e-6));
  TASK_CHECK(close_to(scene->instances[3]->instanceToWorld.end[0][0], 1.0, 1e-6));

  return 0;
}


int loading_errors_and_cycles()
{
  std::string path;
  mkdir_tmp();

  // Undefined object.
  TASK_CHECK(write_temp("tmp_undefined.pbrt",
      "WorldBegin\nObjectBegin \"a\"\nObjectInstance \"missing\"\nObjectEnd\nWorldEnd\n", path));
  {
    Loader loader;
    TASK_CHECK(!loader.load(path.c_str()));
    const Error* error = loader.error();
    TASK_CHECK(error != nullptr);
    TASK_CHECK(error->message() != nullptr);
    TASK_CHECK(std::strstr(error->message(), "undefined") != nullptr);
    TASK_CHECK(error->line() > 0);
    TASK_CHECK(error->filename() != nullptr && error->filename()[0] != '\0');
  }

  // Duplicate object name.
  TASK_CHECK(write_temp("tmp_duplicate.pbrt",
      "WorldBegin\nObjectBegin \"a\"\nObjectEnd\nObjectBegin \"a\"\nObjectEnd\n"
      "ObjectInstance \"a\"\nWorldEnd\n", path));
  {
    Loader loader;
    TASK_CHECK(!loader.load(path.c_str()));
    TASK_CHECK(loader.error() != nullptr);
    TASK_CHECK(std::strstr(loader.error()->message(), "more than once") != nullptr);
    TASK_CHECK(loader.error()->line() > 0);
  }

  // Direct self reference.
  TASK_CHECK(write_temp("tmp_self.pbrt",
      "WorldBegin\nObjectBegin \"a\"\nObjectInstance \"a\"\nObjectEnd\n"
      "ObjectInstance \"a\"\nWorldEnd\n", path));
  {
    Loader loader;
    TASK_CHECK(!loader.load(path.c_str()));
    TASK_CHECK(loader.error() != nullptr);
    TASK_CHECK(std::strstr(loader.error()->message(), "self") != nullptr);
    TASK_CHECK(loader.error()->line() > 0);
  }

  // Indirect cycle.
  TASK_CHECK(write_temp("tmp_cycle.pbrt",
      "WorldBegin\nObjectBegin \"a\"\nObjectInstance \"b\"\nObjectEnd\n"
      "ObjectBegin \"b\"\nObjectInstance \"c\"\nObjectEnd\n"
      "ObjectBegin \"c\"\nObjectInstance \"a\"\nObjectEnd\n"
      "ObjectInstance \"a\"\nWorldEnd\n", path));
  {
    Loader loader;
    TASK_CHECK(!loader.load(path.c_str()));
    TASK_CHECK(loader.error() != nullptr);
    TASK_CHECK(std::strstr(loader.error()->message(), "cycle") != nullptr);
    TASK_CHECK(loader.error()->line() > 0);
  }

  // Include with the failing reference: the error location should name the
  // included file, not the top-level one.
  TASK_CHECK(write_temp("tmp_included_ref.pbrt", "ObjectInstance \"nope\"\n", path));
  TASK_CHECK(write_temp("tmp_include_main.pbrt",
      "WorldBegin\nObjectBegin \"a\"\nInclude \"tmp_included_ref.pbrt\"\nObjectEnd\n"
      "ObjectInstance \"a\"\nWorldEnd\n", path));
  {
    Loader loader;
    TASK_CHECK(!loader.load(path.c_str()));
    const Error* error = loader.error();
    TASK_CHECK(error != nullptr);
    TASK_CHECK(std::strstr(error->filename(), "tmp_included_ref.pbrt") != nullptr);
    TASK_CHECK(std::strstr(error->message(), "undefined") != nullptr);
  }

  return 0;
}


int loading_legacy_single_layer()
{
  std::string path;
  TASK_CHECK(write_temp("tmp_legacy.pbrt",
      "TransformTimes 0 1\nWorldBegin\nObjectBegin \"tri\"\n"
      "Shape \"trianglemesh\" \"integer indices\" [0 1 2] \"point P\" [0 0 0 1 0 0 0 1 0]\n"
      "ObjectEnd\nTranslate 3 4 5\nObjectInstance \"tri\"\nWorldEnd\n", path));

  Loader loader;
  TASK_CHECK(loader.load(path.c_str()));
  Scene* scene = loader.borrow_scene();
  TASK_CHECK(scene != nullptr);
  TASK_CHECK(scene->shapes.size() == 1);
  TASK_CHECK(scene->objects.size() == 1);
  TASK_CHECK(scene->instances.size() == 1);
  TASK_CHECK(scene->instances[0]->ownerObject == kInvalidIndex);
  TASK_CHECK(scene->instances[0]->object == 0);
  TASK_CHECK(scene->objects[0]->firstShape == 0);
  TASK_CHECK(scene->objects[0]->numShapes == 1);

  // Array values and transform numbers are unchanged.
  const TriangleMesh* mesh = static_cast<const TriangleMesh*>(scene->shapes[0]);
  TASK_CHECK(mesh->num_vertices == 3);
  TASK_CHECK(mesh->num_indices == 3);
  TASK_CHECK(mesh->indices[2] == 2);
  TASK_CHECK(mesh->P[3] == 1.0f);
  const Transform& t = scene->instances[0]->instanceToWorld;
  TASK_CHECK(t.start[0][3] == 3.0f);
  TASK_CHECK(t.start[1][3] == 4.0f);
  TASK_CHECK(t.start[2][3] == 5.0f);
  TASK_CHECK(t.end[2][3] == 5.0f);

  return 0;
}


// ---------------------------------------------------------------------------
// Sampling cases
// ---------------------------------------------------------------------------

int sampling_order_and_paths()
{
  Loader loader;
  TASK_CHECK(loader.load(fixture_path("nested_scene.pbrt").c_str()));
  const Scene* scene = loader.borrow_scene();

  std::vector<SceneDraw> draws;
  std::string error = "sentinel";
  TASK_CHECK(sample_scene(*scene, 0.25, draws, error));
  TASK_CHECK(error.empty());
  TASK_CHECK(draws.size() == 5);

  // World-direct shape first, empty path.
  TASK_CHECK(draws[0].shape == 0);
  TASK_CHECK(draws[0].path.empty());

  // Then root instances in order; each object visits its direct shapes before
  // its child instances.
  TASK_CHECK(draws[1].shape == 1);
  TASK_CHECK(draws[1].path.size() == 2 && draws[1].path[0] == 0 && draws[1].path[1] == 3);
  TASK_CHECK(draws[2].shape == 2);
  TASK_CHECK(draws[2].path.size() == 3 &&
             draws[2].path[0] == 0 && draws[2].path[1] == 3 && draws[2].path[2] == 2);
  TASK_CHECK(draws[3].shape == 1);
  TASK_CHECK(draws[3].path.size() == 2 && draws[3].path[0] == 1 && draws[3].path[1] == 4);
  TASK_CHECK(draws[4].shape == 2);
  TASK_CHECK(draws[4].path.size() == 3 &&
             draws[4].path[0] == 1 && draws[4].path[1] == 4 && draws[4].path[2] == 2);

  return 0;
}


int sampling_multiply_order_and_world_vertices()
{
  BuiltScene built;
  build_hierarchy(built);
  const Scene& scene = built.scene;

  std::vector<SceneDraw> draws;
  std::string error;
  TASK_CHECK(sample_scene(scene, 0.5, draws, error));
  TASK_CHECK(draws.size() == 2);
  TASK_CHECK(draws[1].shape == 1);
  TASK_CHECK(draws[1].path.size() == 2);

  // Compare each world element against a separately-computed reference which
  // interpolates first and multiplies second.
  double reference[4][4];
  reference_world(scene, 0.5, reference);
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      TASK_CHECK(close_to(draws[1].world[r][c], reference[r][c], 1e-9));
    }
  }

  // Deliberately show that multiplying the endpoint matrices first gives a
  // different answer: this verifies we are not doing that.
  double rootEnd[4][4], childEnd[4][4], shapeEnd[4][4], wrong[4][4], tmp[4][4];
  auto endpoint = [](const Transform& t, double out[4][4]) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r][c] = t.end[r][c];
  };
  endpoint(scene.instances[0]->instanceToWorld, rootEnd);
  endpoint(scene.instances[1]->instanceToWorld, childEnd);
  endpoint(scene.shapes[1]->shapeToWorld, shapeEnd);
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
    double s = 0.0;
    for (int k = 0; k < 4; k++) s += rootEnd[r][k] * childEnd[k][c];
    tmp[r][c] = s;
  }
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
    double s = 0.0;
    for (int k = 0; k < 4; k++) s += tmp[r][k] * shapeEnd[k][c];
    wrong[r][c] = s;
  }
  bool differs = false;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (!close_to(wrong[r][c], reference[r][c], 1e-6)) differs = true;
  TASK_CHECK(differs);
  // World vertices at the shutter midpoint. Compose the chain by hand:
  //   shape shear at u=0.5:       x' = x + 0.25 y,  y' = y
  //   child "rotation" at u=0.5: element-wise interpolation of the identity
  //       and the 90-degree matrix gives [[0.5,-0.5],[0.5,0.5]] -- note this
  //       is a scaled rotation, not an orthonormal one, so it maps (x,y) to
  //       (0.5x-0.5y, 0.5x+0.5y).
  //   root translation:          x'' = r.x + 1
  // For local vertex 1 = (2,0,0): the shear leaves it at (2,0); the child
  // maps it to (1,1); then the root adds 1 to x, giving (2, 1).
  double worldVertices[9];
  transform_vertices(built.mesh, draws[1].world, worldVertices);
  TASK_CHECK(close_to(worldVertices[3], 2.0, 1e-9));
  TASK_CHECK(close_to(worldVertices[4], 1.0, 1e-9));

  // Normal matrix is the inverse transpose of the world linear part.
  double linear[3][3];
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
      linear[r][c] = draws[1].world[r][c];
  double inverse[3][3];
  invert3(linear, inverse);
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
      TASK_CHECK(close_to(draws[1].normal[r][c], inverse[c][r], 1e-9));

  return 0;
}


int sampling_flip_orientation()
{
  BuiltScene built;
  build_hierarchy(built);
  Scene& scene = built.scene;

  // Turn the shape's own transform into a mirror.
  double mirror[4][4] = {
    { -1.0, 0.0, 0.0, 0.0 },
    {  0.0, 1.0, 0.0, 0.0 },
    {  0.0, 0.0, 1.0, 0.0 },
    {  0.0, 0.0, 0.0, 1.0 },
  };
  set_matrix(built.mesh->shapeToWorld, mirror);

  std::vector<SceneDraw> draws;
  std::string error;

  // Determinant < 0, no explicit flags -> flipped.
  TASK_CHECK(sample_scene(scene, 0.0, draws, error));
  TASK_CHECK(draws[1].flipOrientation == true);

  // Shape's own reverseOrientation XORs in.
  built.mesh->reverseOrientation = true;
  TASK_CHECK(sample_scene(scene, 0.0, draws, error));
  TASK_CHECK(draws[1].flipOrientation == false);

  // And so does an instance flag along the path.
  scene.instances[0]->reverseOrientation = true;
  TASK_CHECK(sample_scene(scene, 0.0, draws, error));
  TASK_CHECK(draws[1].flipOrientation == true);

  // The world-direct shape in this scene is not mirrored.
  TASK_CHECK(draws[0].flipOrientation == false);

  return 0;
}


int sampling_validation_and_failure()
{
  BuiltScene built;
  build_hierarchy(built);
  Scene& scene = built.scene;

  std::vector<SceneDraw> draws;
  std::string error;

  // Non-finite time.
  TASK_CHECK(!sample_scene(scene, std::nan(""), draws, error));
  TASK_CHECK(!error.empty());

  // Non-affine matrix in a reachable shape.
  built.mesh->shapeToWorld.start[3][0] = 1.0f;
  error.clear();
  size_t before = draws.size();
  TASK_CHECK(!sample_scene(scene, 0.5, draws, error));
  TASK_CHECK(!error.empty());
  TASK_CHECK(draws.size() == before);
  built.mesh->shapeToWorld.start[3][0] = 0.0f;

  // Invalid mesh index.
  int saved = built.mesh->indices[0];
  built.mesh->indices[0] = 3;
  error.clear();
  TASK_CHECK(!sample_scene(scene, 0.5, draws, error));
  TASK_CHECK(!error.empty());
  TASK_CHECK(draws.size() == before);
  built.mesh->indices[0] = saved;

  // A reachable non-triangle shape is rejected.
  Sphere* sphere = new Sphere();
  scene.shapes.push_back(sphere);
  built.branch->firstShape = 2;
  built.branch->numShapes = 1;
  sphere->object = 1;
  error.clear();
  TASK_CHECK(!sample_scene(scene, 0.5, draws, error));
  TASK_CHECK(!error.empty());
  TASK_CHECK(draws.size() == before);
  built.branch->firstShape = kInvalidIndex;
  built.branch->numShapes = 0;

  // Unused shapes with non-finite transforms don't matter, but a reachable
  // singular world does.
  double singular[4][4] = {
    { 1.0, 0.0, 0.0, 0.0 },
    { 0.0, 0.0, 0.0, 0.0 },
    { 0.0, 0.0, 1.0, 0.0 },
    { 0.0, 0.0, 0.0, 1.0 },
  };
  set_matrix(built.mesh->shapeToWorld, singular);
  error.clear();
  TASK_CHECK(!sample_scene(scene, 0.5, draws, error));
  TASK_CHECK(!error.empty());
  TASK_CHECK(draws.size() == before);

  return 0;
}


int sampling_out_of_range_clamping()
{
  BuiltScene built;
  build_hierarchy(built);
  Scene& scene = built.scene;

  std::vector<SceneDraw> draws;
  std::string error;
  double referenceAtStart[4][4];
  double referenceAtEnd[4][4];
  reference_world(scene, 0.0, referenceAtStart);
  reference_world(scene, 1.0, referenceAtEnd);

  TASK_CHECK(sample_scene(scene, -5.0, draws, error));
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      TASK_CHECK(close_to(draws[1].world[r][c], referenceAtStart[r][c], 1e-9));

  TASK_CHECK(sample_scene(scene, 5.0, draws, error));
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      TASK_CHECK(close_to(draws[1].world[r][c], referenceAtEnd[r][c], 1e-9));

  // Equal start and end times: the start matrix is used.
  scene.startTime = 2.0f;
  scene.endTime = 2.0f;
  TASK_CHECK(sample_scene(scene, 7.0, draws, error));
  reference_world(scene, 0.0, referenceAtStart);
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      TASK_CHECK(close_to(draws[1].world[r][c], referenceAtStart[r][c], 1e-9));

  return 0;
}


// ---------------------------------------------------------------------------
// Bounds cases
// ---------------------------------------------------------------------------

// Build a single world-direct mesh, with a shape transform whose translation
// is a quadratic/cubic in time. Attained by composing two/three animated
// translations with constants, so the result is a genuine polynomial of
// degree 2 or 3 in u.
struct PolyScene {
  Scene scene;
  TriangleMesh* mesh;
};

void build_polynomial_scene(PolyScene& out, double a, double b, double c, double d)
{
  Scene& scene = out.scene;
  scene.startTime = 0.0f;
  scene.endTime = 1.0f;

  TriangleMesh* mesh = new TriangleMesh();
  mesh->num_vertices = 1;
  mesh->P = new float[3];
  mesh->P[0] = 1.0f;
  mesh->P[1] = 0.0f;
  mesh->P[2] = 0.0f;
  mesh->num_indices = 3;
  mesh->indices = new int[3];
  mesh->indices[0] = mesh->indices[1] = mesh->indices[2] = 0;
  scene.shapes.push_back(mesh);
  mesh->object = kInvalidIndex;

  // Shape transform: translation along x goes from 0 to a.
  set_identity(mesh->shapeToWorld);
  mesh->shapeToWorld.start[0][3] = 0.0f;
  mesh->shapeToWorld.end[0][3] = float(a);

  // Attach the mesh to a single object so the scene has a complete hierarchy.
  Object* object = new Object();
  object->name = new char[4];
  std::strcpy(const_cast<char*>(object->name), "obj");
  object->firstShape = 0;
  object->numShapes = 1;
  set_identity(object->objectToInstance);
  scene.objects.push_back(object);
  mesh->object = 0;

  // Instance: x-scale goes from 1 to (1 + b), which is linear in u.
  Instance* instance = new Instance();
  instance->object = 0;
  instance->ownerObject = kInvalidIndex;
  set_identity(instance->instanceToWorld);
  instance->instanceToWorld.start[0][0] = 1.0f;
  instance->instanceToWorld.end[0][0] = float(1.0 + b);
  instance->instanceToWorld.start[0][3] = 0.0f;
  instance->instanceToWorld.end[0][3] = float(c);
  scene.instances.push_back(instance);

  // The mesh vertex is at x = 1. Its world x is
  //   scale(u) * (1 + a*u) + translation(u)
  // = (1 + b u)(1 + a u) + c u
  // = 1 + (a + b + c) u + a b u^2.
  (void)d;
  out.mesh = mesh;
}

// Verify a bound against a dense independent sampling plus a closed-form
// extremum for a quadratic. This deliberately does not call sample_scene.
int bounds_quadratic_reference()
{
  // x(u) = 1 + p u + q u^2 with p = a + b + c, q = a*b.
  // a = 2, b = 0.5, c = -3.5 gives x(u) = 1 - u + u^2, whose minimum is at
  // the interior point u = 0.5 (value 0.75) while both endpoints equal 1.
  // Endpoint-only reasoning would therefore report the wrong minimum.
  const double a = 2.0;
  const double b = 0.5;
  const double c = -3.5;
  const double p = a + b + c;
  const double q = a * b;

  PolyScene poly;
  build_polynomial_scene(poly, a, b, c, 0.0);

  const double epsilon = 1e-7;
  std::vector<SceneMotionBounds> bounds;
  std::string error;
  TASK_CHECK(bound_scene_motion(poly.scene, 0.0, 1.0, epsilon, bounds, error));
  TASK_CHECK(error.empty());
  TASK_CHECK(bounds.size() == 1);
  TASK_CHECK(bounds[0].shape == 0);

  // Independent closed-form extrema of 1 + p u + q u^2 on [0,1].
  double minValue = 1.0;
  double maxValue = 1.0;
  double values[2] = { 1.0, 1.0 + p + q };
  for (int i = 0; i < 2; i++) {
    if (values[i] < minValue) minValue = values[i];
    if (values[i] > maxValue) maxValue = values[i];
  }
  double critical = -p / (2.0 * q);
  if (critical > 0.0 && critical < 1.0) {
    double v = 1.0 + p * critical + q * critical * critical;
    if (v < minValue) minValue = v;
    if (v > maxValue) maxValue = v;
  }

  double scale = std::max(1.0, std::max(std::fabs(minValue), std::fabs(maxValue)));
  double tolerance = epsilon + 1e-8 * scale;
  TASK_CHECK(bounds[0].min[0] <= minValue + 1e-12);
  TASK_CHECK(bounds[0].max[0] >= maxValue - 1e-12);
  TASK_CHECK(bounds[0].min[0] - minValue <= tolerance);
  TASK_CHECK(bounds[0].max[0] - maxValue <= tolerance);

  // The y and z trajectories are constant zero.
  TASK_CHECK(close_to(bounds[0].min[1], 0.0, tolerance));
  TASK_CHECK(close_to(bounds[0].max[1], 0.0, tolerance));

  return 0;
}

int bounds_cubic_reference()
{
  // Build a genuine cubic world trajectory for a single vertex and compare the
  // bound against an independent closed-form extremum.
  //
  // Chain: outer instance * inner instance * shapeToWorld, applied to the
  // local point (1, 0, 0). Choose
  //   shape x translation : t(u) = 5 - 2u            (so 1 + t = 6 - 2u)
  //   inner instance scale: 1 + k u with k = -0.5    (1 -> 0.5)
  //   outer instance scale: 1 + m u with m = +1      (1 -> 2)
  // Then the world x-coordinate is
  //   x(u) = (1 + u)(1 - 0.5u)(6 - 2u)
  //        = (u + 1)(u - 2)(u - 3)
  //        = u^3 - 4u^2 + u + 6.
  // Its derivative is 3u^2 - 8u + 1, which has a root inside (0,1):
  //   u* = (8 - sqrt(52)) / 6 ~= 0.13148,
  // giving an interior maximum well above both endpoint values (x(0)=6,
  // x(1)=4). Endpoint-only reasoning would miss it.

  Scene scene;
  scene.startTime = 0.0f;
  scene.endTime = 1.0f;

  TriangleMesh* mesh = new TriangleMesh();
  mesh->num_vertices = 1;
  mesh->P = new float[3];
  mesh->P[0] = 1.0f;
  mesh->P[1] = 0.0f;
  mesh->P[2] = 0.0f;
  mesh->num_indices = 3;
  mesh->indices = new int[3];
  mesh->indices[0] = mesh->indices[1] = mesh->indices[2] = 0;
  scene.shapes.push_back(mesh);

  set_identity(mesh->shapeToWorld);
  mesh->shapeToWorld.start[0][3] = 5.0f;
  mesh->shapeToWorld.end[0][3] = 3.0f;

  Object* leaf = new Object();
  leaf->name = new char[5];
  std::strcpy(const_cast<char*>(leaf->name), "leaf");
  leaf->firstShape = 0;
  leaf->numShapes = 1;
  set_identity(leaf->objectToInstance);
  scene.objects.push_back(leaf);
  mesh->object = 0;

  Object* outer = new Object();
  outer->name = new char[6];
  std::strcpy(const_cast<char*>(outer->name), "outer");
  outer->firstShape = kInvalidIndex;
  outer->numShapes = 0;
  set_identity(outer->objectToInstance);
  scene.objects.push_back(outer);

  Instance* inner = new Instance();
  inner->object = 0;
  inner->ownerObject = 1;
  set_identity(inner->instanceToWorld);
  inner->instanceToWorld.start[0][0] = 1.0f;
  inner->instanceToWorld.end[0][0] = 0.5f;
  scene.instances.push_back(inner);

  Instance* outerInstance = new Instance();
  outerInstance->object = 1;
  outerInstance->ownerObject = kInvalidIndex;
  set_identity(outerInstance->instanceToWorld);
  outerInstance->instanceToWorld.start[0][0] = 1.0f;
  outerInstance->instanceToWorld.end[0][0] = 2.0f;
  scene.instances.push_back(outerInstance);

  // Independent closed-form extrema of x(u) = u^3 - 4u^2 + u + 6 on [0,1].
  auto cubic = [](double u) { return ((u - 4.0) * u + 1.0) * u + 6.0; };
  double mn = std::min(cubic(0.0), cubic(1.0));
  double mx = std::max(cubic(0.0), cubic(1.0));
  {
    double disc = 64.0 - 12.0;
    double root = (8.0 - std::sqrt(disc)) / 6.0;
    if (root > 0.0 && root < 1.0) {
      double v = cubic(root);
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
  }
  // Sanity: the interior maximum must exceed both endpoints, otherwise the
  // test would not distinguish a tight bound from an endpoint-only bound.
  TASK_CHECK(mx > cubic(0.0) + 0.01);
  TASK_CHECK(mx > cubic(1.0) + 0.01);

  const double epsilon = 1e-7;
  std::vector<SceneMotionBounds> bounds;
  std::string error;
  TASK_CHECK(bound_scene_motion(scene, 0.0, 1.0, epsilon, bounds, error));
  TASK_CHECK(error.empty());
  TASK_CHECK(bounds.size() == 1);

  double scale = std::max(1.0, std::max(std::fabs(mn), std::fabs(mx)));
  double tolerance = epsilon + 1e-8 * scale;
  TASK_CHECK(bounds[0].min[0] <= mn + 1e-9);
  TASK_CHECK(bounds[0].max[0] >= mx - 1e-9);
  TASK_CHECK(bounds[0].min[0] - mn <= tolerance);
  TASK_CHECK(bounds[0].max[0] - mx <= tolerance);

  return 0;
}


int bounds_instant_and_clamped()
{
  BuiltScene built;
  build_hierarchy(built);
  const Scene& scene = built.scene;

  std::vector<SceneMotionBounds> bounds;
  std::string error;

  // Instantaneous interval: the box must contain the world vertices at that
  // single time.
  std::vector<SceneDraw> draws;
  TASK_CHECK(sample_scene(scene, 0.25, draws, error));
  TASK_CHECK(draws.size() == 2);
  double worldVertices[9];
  transform_vertices(built.mesh, draws[1].world, worldVertices);
  TASK_CHECK(bound_scene_motion(scene, 0.25, 0.25, 1e-6, bounds, error));
  TASK_CHECK(error.empty());
  TASK_CHECK(bounds.size() == 2);
  for (int v = 0; v < 3; v++) {
    for (int axis = 0; axis < 3; axis++) {
      TASK_CHECK(bounds[1].min[axis] <= worldVertices[v * 3 + axis] + 1e-9);
      TASK_CHECK(bounds[1].max[axis] >= worldVertices[v * 3 + axis] - 1e-9);
    }
  }

  // Out-of-range interval clamps exactly like sampling does.
  std::vector<SceneMotionBounds> clamped;
  TASK_CHECK(bound_scene_motion(scene, -10.0, 10.0, 1e-6, clamped, error));
  TASK_CHECK(error.empty());
  std::vector<SceneMotionBounds> normal;
  TASK_CHECK(bound_scene_motion(scene, 0.0, 1.0, 1e-6, normal, error));
  TASK_CHECK(clamped.size() == normal.size());
  for (size_t i = 0; i < clamped.size(); i++) {
    for (int axis = 0; axis < 3; axis++) {
      TASK_CHECK(close_to(clamped[i].min[axis], normal[i].min[axis], 1e-7));
      TASK_CHECK(close_to(clamped[i].max[axis], normal[i].max[axis], 1e-7));
    }
  }

  // Invalid epsilon and inverted interval are rejected without touching out.
  std::vector<SceneMotionBounds> sentinel = normal;
  error.clear();
  TASK_CHECK(!bound_scene_motion(scene, 0.0, 1.0, 0.0, sentinel, error));
  TASK_CHECK(!error.empty());
  TASK_CHECK(sentinel.size() == normal.size());
  error.clear();
  TASK_CHECK(!bound_scene_motion(scene, 1.0, 0.0, 1e-6, sentinel, error));
  TASK_CHECK(!error.empty());

  return 0;
}

int bounds_degenerate_and_empty()
{
  // A world-direct mesh whose x-scale degenerates to zero at the end of the
  // interval. The bound must still succeed and enclose the trajectory.
  Scene scene;
  scene.startTime = 0.0f;
  scene.endTime = 1.0f;

  TriangleMesh* mesh = new TriangleMesh();
  mesh->num_vertices = 2;
  mesh->P = new float[6];
  mesh->P[0] = 5.0f; mesh->P[1] = 2.0f; mesh->P[2] = -3.0f;
  mesh->P[3] = -4.0f; mesh->P[4] = 6.0f; mesh->P[5] = 1.0f;
  mesh->num_indices = 3;
  mesh->indices = new int[3];
  mesh->indices[0] = mesh->indices[1] = mesh->indices[2] = 0;
  scene.shapes.push_back(mesh);
  mesh->object = kInvalidIndex;
  set_identity(mesh->shapeToWorld);
  mesh->shapeToWorld.start[0][0] = 1.0f;
  mesh->shapeToWorld.end[0][0] = 0.0f;

  std::vector<SceneMotionBounds> bounds;
  std::string error;
  TASK_CHECK(bound_scene_motion(scene, 0.0, 1.0, 1e-6, bounds, error));
  TASK_CHECK(error.empty());
  TASK_CHECK(bounds.size() == 1);
  TASK_CHECK(bounds[0].min[0] <= -4.0 + 1e-9);
  TASK_CHECK(bounds[0].max[0] >= 5.0 - 1e-9);
  TASK_CHECK(bounds[0].min[1] <= 2.0 + 1e-9 && bounds[0].max[1] >= 6.0 - 1e-9);
  TASK_CHECK(bounds[0].min[2] <= -3.0 + 1e-9 && bounds[0].max[2] >= 1.0 - 1e-9);

  // An empty object is legal, and a scene with no reachable geometry yields an
  // empty result.
  Scene emptyScene;
  emptyScene.startTime = 0.0f;
  emptyScene.endTime = 1.0f;
  Object* emptyObject = new Object();
  emptyObject->name = new char[6];
  std::strcpy(const_cast<char*>(emptyObject->name), "empty");
  emptyObject->firstShape = kInvalidIndex;
  emptyObject->numShapes = 0;
  set_identity(emptyObject->objectToInstance);
  emptyScene.objects.push_back(emptyObject);
  std::vector<SceneMotionBounds> emptyBounds;
  error.clear();
  TASK_CHECK(bound_scene_motion(emptyScene, 0.0, 1.0, 1e-6, emptyBounds, error));
  TASK_CHECK(error.empty());
  TASK_CHECK(emptyBounds.empty());

  std::vector<SceneDraw> emptyDraws;
  TASK_CHECK(sample_scene(emptyScene, 0.5, emptyDraws, error));
  TASK_CHECK(emptyDraws.empty());

  return 0;
}

} // namespace


// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

extern const struct task_case task_cases[] = {
  { "loading", "forward_diamond",       loading_forward_diamond },
  { "loading", "errors_and_cycles",     loading_errors_and_cycles },
  { "loading", "legacy_single_layer",   loading_legacy_single_layer },
  { "sampling", "order_and_paths",      sampling_order_and_paths },
  { "sampling", "multiply_order",       sampling_multiply_order_and_world_vertices },
  { "sampling", "flip_orientation",     sampling_flip_orientation },
  { "sampling", "validation_failure",   sampling_validation_and_failure },
  { "sampling", "clamping",             sampling_out_of_range_clamping },
  { "bounds", "quadratic_reference",    bounds_quadratic_reference },
  { "bounds", "cubic_reference",        bounds_cubic_reference },
  { "bounds", "instant_and_clamped",    bounds_instant_and_clamped },
  { "bounds", "degenerate_and_empty",   bounds_degenerate_and_empty },
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
