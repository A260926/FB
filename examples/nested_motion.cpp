// nested_motion - demonstrate shutter-time sampling and continuous motion
// bounds for a scene that reuses an animated object hierarchy.
//
// The example writes a small PBRT file to a temporary path, loads it with
// minipbrt, samples the scene at the middle of the shutter interval, and then
// prints a continuous motion bound for the whole interval.

#include "minipbrt_scene.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string temporary_pbrt_path()
{
  const char* tmpDir = std::getenv("TMPDIR");
  if (tmpDir == nullptr || tmpDir[0] == '\0') {
    tmpDir = ".";
  }
  std::string path = tmpDir;
  if (!path.empty() && path[path.size() - 1] != '/') {
    path += '/';
  }
  path += "minipbrt_nested_motion_example.pbrt";
  return path;
}

bool write_scene(const std::string& path)
{
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "Failed to open %s for writing\n", path.c_str());
    return false;
  }

  // "arm" is an animated object that instanceToWorld rotates about z from 0 to
  // 60 degrees. "hub" contains its own animated translate and instantiates
  // "arm" twice, sharing the same geometry. The world instantiates "hub".
  const char* contents =
      "TransformTimes 0 1\n"
      "WorldBegin\n"
      "  Shape \"trianglemesh\"\n"
      "    \"integer indices\" [0 1 2]\n"
      "    \"point P\" [0 0 0  1 0 0  0 1 0]\n"
      "  ObjectBegin \"arm\"\n"
      "    ActiveTransform EndTime\n"
      "      Rotate 60 0 0 1\n"
      "    ActiveTransform All\n"
      "    Shape \"trianglemesh\"\n"
      "      \"integer indices\" [0 1 2]\n"
      "      \"point P\" [0 0 0  2 0 0  0 1 0]\n"
      "  ObjectEnd\n"
      "  ObjectBegin \"hub\"\n"
      "    ActiveTransform EndTime\n"
      "      Translate 1 0 0\n"
      "    ActiveTransform All\n"
      "    ObjectInstance \"arm\"\n"
      "    AttributeBegin\n"
      "      Translate 0 2 0\n"
      "      ObjectInstance \"arm\"\n"
      "    AttributeEnd\n"
      "  ObjectEnd\n"
      "  ObjectInstance \"hub\"\n"
      "WorldEnd\n";

  bool ok = std::fputs(contents, f) >= 0;
  std::fclose(f);
  return ok;
}

void print_matrix(const double m[4][4])
{
  for (int r = 0; r < 3; r++) {
    std::printf("    ");
    for (int c = 0; c < 4; c++) {
      std::printf("%9.4f ", m[r][c]);
    }
    std::printf("\n");
  }
}

void print_path(const std::vector<uint32_t>& path)
{
  std::printf("[");
  for (size_t i = 0; i < path.size(); i++) {
    std::printf("%s%u", (i == 0) ? "" : ",", path[i]);
  }
  std::printf("]");
}

} // namespace


int main()
{
  const std::string path = temporary_pbrt_path();
  if (!write_scene(path)) {
    return 1;
  }

  minipbrt::Loader loader;
  if (!loader.load(path.c_str())) {
    const minipbrt::Error* error = loader.error();
    if (error != nullptr) {
      std::fprintf(stderr, "[%s, line %lld] %s\n",
                   error->filename(), static_cast<long long>(error->line()),
                   error->message());
    }
    return 1;
  }

  const minipbrt::Scene* scene = loader.borrow_scene();
  std::printf("Loaded %s: %u shapes, %u objects, %u instances\n",
              path.c_str(),
              static_cast<unsigned>(scene->shapes.size()),
              static_cast<unsigned>(scene->objects.size()),
              static_cast<unsigned>(scene->instances.size()));

  // Mid-shutter sample: print every world-space triangle-mesh vertex.
  std::vector<minipbrt::SceneDraw> draws;
  std::string error;
  if (!minipbrt::sample_scene(*scene, 0.5, draws, error)) {
    std::fprintf(stderr, "sample_scene failed: %s\n", error.c_str());
    return 1;
  }

  std::printf("\nMid-shutter (t = 0.5) draws: %u\n",
              static_cast<unsigned>(draws.size()));
  for (size_t i = 0; i < draws.size(); i++) {
    const minipbrt::SceneDraw& draw = draws[i];
    std::printf("  draw %u: shape %u path ", static_cast<unsigned>(i), draw.shape);
    print_path(draw.path);
    std::printf(" flipOrientation=%d\n", draw.flipOrientation ? 1 : 0);
    std::printf("  world:\n");
    print_matrix(draw.world);

    const minipbrt::TriangleMesh* mesh =
        static_cast<const minipbrt::TriangleMesh*>(scene->shapes[draw.shape]);
    std::printf("  world vertices:");
    for (unsigned int v = 0; v < mesh->num_vertices; v++) {
      double x = draw.world[0][0] * mesh->P[v * 3 + 0] +
                 draw.world[0][1] * mesh->P[v * 3 + 1] +
                 draw.world[0][2] * mesh->P[v * 3 + 2] + draw.world[0][3];
      double y = draw.world[1][0] * mesh->P[v * 3 + 0] +
                 draw.world[1][1] * mesh->P[v * 3 + 1] +
                 draw.world[1][2] * mesh->P[v * 3 + 2] + draw.world[1][3];
      double z = draw.world[2][0] * mesh->P[v * 3 + 0] +
                 draw.world[2][1] * mesh->P[v * 3 + 1] +
                 draw.world[2][2] * mesh->P[v * 3 + 2] + draw.world[2][3];
      std::printf(" (%.4f, %.4f, %.4f)", x, y, z);
    }
    std::printf("\n");
  }

  // Continuous bounds over the whole shutter interval.
  std::vector<minipbrt::SceneMotionBounds> bounds;
  error.clear();
  if (!minipbrt::bound_scene_motion(*scene, 0.0, 1.0, 1e-4, bounds, error)) {
    std::fprintf(stderr, "bound_scene_motion failed: %s\n", error.c_str());
    return 1;
  }

  std::printf("\nContinuous motion bounds over [0, 1]: %u\n",
              static_cast<unsigned>(bounds.size()));
  for (size_t i = 0; i < bounds.size(); i++) {
    const minipbrt::SceneMotionBounds& bound = bounds[i];
    std::printf("  bound %u: shape %u path ", static_cast<unsigned>(i), bound.shape);
    print_path(bound.path);
    std::printf("\n    min = (%.4f, %.4f, %.4f)\n", bound.min[0], bound.min[1], bound.min[2]);
    std::printf("    max = (%.4f, %.4f, %.4f)\n", bound.max[0], bound.max[1], bound.max[2]);
  }

  std::remove(path.c_str());
  return 0;
}
