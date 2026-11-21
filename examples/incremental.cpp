// Incremental preview example.
//
// Renders a scene with the incremental engine enabled, applies a batch
// of sphere edits (move + recolor), and re-renders. Only the queries
// affected by the edit are replayed; the result is identical to a full
// re-trace. Saves before.ppm / after.ppm into the given output
// directory (default: current directory) and prints the frame stats.

#include "camera.hpp"
#include "light.hpp"
#include "ppm_writer.hpp"
#include "ray_tracer.hpp"
#include "ray_cache.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  const std::string out_dir = (argc > 1 && argv[1]) ? argv[1] : ".";

  Camera cam(64, 60, 60, {0, 0, -400});
  Lights lights;
  lights.AddAmbient(0.4f);
  lights.AddPoint(0.6f, 300, 200, -100);
  RayTracer tracer(cam, lights);
  tracer.SetBackground(18, 22, 30);

  Material matte;
  matte.color = {200, 90, 60};
  matte.specular = 20;
  matte.reflective = 0.0f;
  matte.transparency = 0.0f;
  matte.refractive_index = 1.0f;
  matte.tint = 0.1f;
  matte.emission = {0, 0, 0};
  matte.emit_strength = 0.0f;

  Material mirror = matte;
  mirror.color = {230, 230, 230};
  mirror.specular = 80;
  mirror.reflective = 0.6f;

  Material glass = matte;
  glass.color = {240, 250, 255};
  glass.specular = 60;
  glass.reflective = 0.15f;
  glass.transparency = 0.6f;
  glass.refractive_index = 1.4f;
  glass.tint = 0.2f;

  Sphere& s1 = tracer.AddObject<Sphere>(Vec3f{-160, 0, 700}, 120, matte);
  Sphere& s2 = tracer.AddObject<Sphere>(Vec3f{180, -40, 500}, 90, mirror);
  Sphere& s3 = tracer.AddObject<Sphere>(Vec3f{0, 120, 900}, 140, glass);
  (void)s1;

  AABB domain;
  domain.min = {-1200, -1200, -300};
  domain.max = {1200, 1200, 1400};
  if (!tracer.EnableIncremental(domain)) {
    std::cerr << "EnableIncremental failed" << std::endl;
    return 1;
  }

  tracer.Trace(4);
  Ppm::Write(tracer.image(), out_dir + "/before.ppm");
  const auto cold = tracer.IncrementalStats();
  std::cout << "cold frame: created=" << cold.created_nodes
            << " object_tests=" << cold.object_tests << std::endl;

  // batch edit: move the mirror sphere, recolor the glass sphere
  Material glass2 = glass;
  glass2.color = {255, 200, 220};
  std::vector<incremental::SphereEdit> edits;
  edits.push_back({&s2, Vec3f{60, -20, 500}, 90, mirror});
  edits.push_back({&s3, Vec3f{0, 120, 900}, 140, glass2});
  if (!tracer.ApplySphereEdits(edits)) {
    std::cerr << "ApplySphereEdits rejected" << std::endl;
    return 2;
  }
  tracer.Trace(4);
  Ppm::Write(tracer.image(), out_dir + "/after.ppm");

  const auto st = tracer.IncrementalStats();
  std::cout << "incremental frame: ray_replayed=" << st.ray_nodes_replayed
            << " shadow_replayed=" << st.shadow_queries_replayed
            << " probe_replayed=" << st.probe_queries_replayed
            << " pixel_reductions=" << st.pixel_reductions
            << " created=" << st.created_nodes
            << " retired=" << st.retired_nodes
            << " object_tests=" << st.object_tests
            << " index_visited=" << st.index_nodes_visited << std::endl;
  return 0;
}
