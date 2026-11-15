// Beam-traversal demo.
//
// Builds a 65x49 scene consisting of a near Quad that covers the whole
// view and a 64x64 grid of small, far spheres that are completely
// hidden behind it, then renders it with the CPU primary-ray beam
// traversal enabled and prints the real work counters.
//
// Usage: beam <output.ppm>

#include "ray_tracer.hpp"
#include "ppm_writer.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

// Camera depth 5.  Camera stores the plane size as
// 2 * depth * tan(fov / 2), so a half-width of 32.5 and a half-height
// of 24.5 produce exactly a 65x49 image.
Camera MakeCamera() {
  constexpr float depth = 5.0f;
  const float fovx = 2.0f * std::atan(32.5f / depth) * 180.0f /
                     static_cast<float>(M_PI) + 1e-3f;
  const float fovy = 2.0f * std::atan(24.5f / depth) * 180.0f /
                     static_cast<float>(M_PI) + 1e-3f;
  return Camera(depth, fovx, fovy, Vec3f{0, 0, 0}, Mat3x3{});
}

} // namespace

int main(int argc, char** argv) {
  const std::string output = argc > 1 ? argv[1] : "beam.ppm";

  Camera camera = MakeCamera();
  Lights lights;
  lights.AddAmbient(0.5f);
  lights.AddDir(0.5f, 0.3f, -0.4f, -1.0f);

  RayTracer tracer(camera, lights);
  tracer.SetBackground(4, 6, 9);

  const Material cover_material{[] {
    Material m;
    m.color = {200, 90, 40};
    m.specular = 0.0f;
    m.reflective = 0.0f;
    m.transparency = 0.0f;
    return m;
  }()};
  const Material hidden_material{[] {
    Material m;
    m.color = {40, 200, 110};
    m.specular = 0.0f;
    m.reflective = 0.0f;
    m.transparency = 0.0f;
    return m;
  }()};

  // A single near Quad spanning far beyond the view at z = 10.
  tracer.AddObject<Quad>(Vec3f{-80, -80, 10}, Vec3f{80, -80, 10},
                         Vec3f{80, 80, 10}, Vec3f{-80, 80, 10},
                         cover_material);

  // 4096 small spheres far behind the cover, spread over a wide area
  // so the object BVH has real depth structure to exploit.
  constexpr int kGrid = 64;
  for (int iy = 0; iy < kGrid; ++iy) {
    for (int ix = 0; ix < kGrid; ++ix) {
      const float x = -40.0f + static_cast<float>(ix) * (80.0f / 63.0f);
      const float y = -40.0f + static_cast<float>(iy) * (80.0f / 63.0f);
      tracer.AddObject<Sphere>(Vec3f{x, y, 80.0f}, 0.3f, hidden_material);
    }
  }

  tracer.SetBeamTraversal(true);
  tracer.Trace(1);

  Ppm::Write(tracer.image(), output);

  const beam::WorkStats& stats = tracer.BeamStats();
  std::printf("beam: %zux%zu objects=%zu\n", camera.width(), camera.height(),
              static_cast<std::size_t>(1 + kGrid * kGrid));
  std::printf("beam: beam_box_tests=%llu depth_rejects=%llu object_tests=%llu\n",
              static_cast<unsigned long long>(stats.beam_box_tests),
              static_cast<unsigned long long>(stats.depth_rejects),
              static_cast<unsigned long long>(stats.object_tests));
  return 0;
}
