#include "minipbrt_scene.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string write_example_scene() {
  const char* tmpDir = std::getenv("TMPDIR");
  std::string dir = (tmpDir != nullptr && tmpDir[0] != '\0') ? tmpDir : ".task-build";
  if (!dir.empty() && dir[dir.size() - 1] != '/') {
    dir.push_back('/');
  }
  std::string path = dir + "minipbrt_nested_motion_example.pbrt";

  std::ofstream out(path.c_str());
  out <<
    "TransformTimes 0 1\n"
    "WorldBegin\n"
    "ObjectBegin \"leaf\"\n"
    "  Shape \"trianglemesh\" \"integer indices\" [0 1 2]\n"
    "    \"point P\" [0 0 0  1 0 0  0 1 0]\n"
    "ObjectEnd\n"
    "ObjectBegin \"group\"\n"
    "  ActiveTransform StartTime\n"
    "  Translate 0 0 0\n"
    "  ActiveTransform EndTime\n"
    "  Translate 1 0 0\n"
    "  ActiveTransform All\n"
    "  ObjectInstance \"leaf\"\n"
    "ObjectEnd\n"
    "ActiveTransform StartTime\n"
    "Scale 2 1 1\n"
    "ActiveTransform EndTime\n"
    "Scale 1 1 1\n"
    "ActiveTransform All\n"
    "ObjectInstance \"group\"\n"
    "WorldEnd\n";
  return path;
}

void transform_vertex(const double world[4][4], const float p[3], double q[3]) {
  for (int i = 0; i < 3; ++i) {
    q[i] = world[i][0] * p[0] + world[i][1] * p[1] +
           world[i][2] * p[2] + world[i][3];
  }
}

} // namespace

int main() {
  std::string path = write_example_scene();

  minipbrt::Loader loader;
  if (!loader.load(path.c_str())) {
    const minipbrt::Error* error = loader.error();
    if (error != nullptr) {
      std::fprintf(stderr, "example load failed: %s:%lld: %s\n",
                   error->filename(), static_cast<long long>(error->line()),
                   error->message());
    }
    return 1;
  }

  const minipbrt::Scene* scene = loader.borrow_scene();
  std::vector<minipbrt::SceneDraw> draws;
  std::string error;
  if (!minipbrt::sample_scene(*scene, 0.5, draws, error)) {
    std::fprintf(stderr, "sample_scene failed: %s\n", error.c_str());
    return 1;
  }

  std::printf("Loaded %s\n", path.c_str());
  std::printf("Mid-shutter (t=0.5) transformed vertices:\n");
  for (size_t d = 0; d < draws.size(); ++d) {
    const minipbrt::SceneDraw& draw = draws[d];
    if (draw.shape >= scene->shapes.size() ||
        scene->shapes[draw.shape]->type() != minipbrt::ShapeType::TriangleMesh) {
      std::fprintf(stderr, "example expected a triangle mesh\n");
      return 1;
    }
    const minipbrt::TriangleMesh* mesh =
      static_cast<const minipbrt::TriangleMesh*>(scene->shapes[draw.shape]);

    std::printf("draw shape=%u path=[", draw.shape);
    for (size_t i = 0; i < draw.path.size(); ++i) {
      if (i) std::printf(", ");
      std::printf("%u", draw.path[i]);
    }
    std::printf("] flip=%d\n", static_cast<int>(draw.flipOrientation));

    for (unsigned int v = 0; v < mesh->num_vertices; ++v) {
      float p[3] = {
        mesh->P[v * 3u + 0u],
        mesh->P[v * 3u + 1u],
        mesh->P[v * 3u + 2u]
      };
      double q[3];
      transform_vertex(draw.world, p, q);
      std::printf("  vertex %u: (%.3f %.3f %.3f) -> (%.6f %.6f %.6f)\n",
                  v, p[0], p[1], p[2], q[0], q[1], q[2]);
    }
  }

  std::vector<minipbrt::SceneMotionBounds> bounds;
  if (!minipbrt::bound_scene_motion(*scene, 0.0, 1.0, 1e-7, bounds, error)) {
    std::fprintf(stderr, "bound_scene_motion failed: %s\n", error.c_str());
    return 1;
  }

  std::printf("Continuous motion bounds over [0,1]:\n");
  for (size_t i = 0; i < bounds.size(); ++i) {
    const minipbrt::SceneMotionBounds& b = bounds[i];
    std::printf("shape=%u path=[", b.shape);
    for (size_t j = 0; j < b.path.size(); ++j) {
      if (j) std::printf(", ");
      std::printf("%u", b.path[j]);
    }
    std::printf("] min=(%.6f %.6f %.6f) max=(%.6f %.6f %.6f)\n",
                b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2]);
  }

  if (draws.size() != 1 || bounds.size() != 1) {
    std::fprintf(stderr, "example expected one draw and one bounds entry\n");
    return 1;
  }
  const double eps = 1e-5;
  if (std::fabs(draws[0].world[0][3] - 0.75) > eps ||
      std::fabs(bounds[0].max[0] - 2.25) > 1e-7) {
    std::fprintf(stderr, "example numerical output did not match the scene\n");
    return 1;
  }

  std::printf("EXAMPLE_OK nested draws=%u bounds=%u\n",
              static_cast<unsigned>(draws.size()),
              static_cast<unsigned>(bounds.size()));
  return 0;
}
