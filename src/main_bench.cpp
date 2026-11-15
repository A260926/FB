// Throughput of the immediate reference vs the tiled path at 1 and N threads: a geometry-bound
// torus, a fill-bound stack of textured layers, and the overdraw-heavy depth stress scene.
#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "softgpu/args.hpp"
#include "softgpu/scenes.hpp"

using namespace softgpu;

namespace {

struct Run {
  double ms;
  Stats stats;
};

Run bench_scene(const SceneOptions& opt, int w, int h, Mode mode, int threads, int tile, int frames) {
  Renderer r(w, h, Config{mode, threads, tile});
  render_scene(r, opt);  // warm-up: texture statics, thread pool, page faults
  Run best{1e18, {}};
  for (int f = 0; f < frames; ++f) {
    r.stats().reset();
    render_scene(r, opt);
    const double ms = r.stats().total_ns / 1e6;
    if (ms < best.ms) best = {ms, r.stats()};
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  Args args(argc, argv);
  const int w = static_cast<int>(args.num("width", 1920)), h = static_cast<int>(args.num("height", 1080));
  const int frames = static_cast<int>(args.num("frames", 5));
  const int tile = static_cast<int>(args.num("tile", 32));
  const int nthreads = static_cast<int>(args.num("threads", static_cast<long>(std::thread::hardware_concurrency())));

  struct Case {
    const char* name;
    SceneOptions opt;
  };
  std::vector<Case> cases;
  SceneOptions torus;
  torus.name = "torus";
  torus.detail = static_cast<int>(args.num("detail", 6));  // 9216 * detail^2 triangles
  cases.push_back({"torus", torus});
  SceneOptions fill;
  fill.name = "fill";
  fill.count = static_cast<int>(args.num("layers", 8)) * 250;
  fill.filter = Filter::Bilinear;
  cases.push_back({"fill", fill});
  SceneOptions depth;
  depth.name = "depth";
  depth.count = static_cast<int>(args.num("count", 20000));
  cases.push_back({"depth", depth});
  if (args.has("scene")) {
    const std::string only = args.str("scene", "");
    cases.erase(std::remove_if(cases.begin(), cases.end(), [&](const Case& c) { return only != c.name; }), cases.end());
  }

  std::printf("%dx%d, tile %d, best of %d frames, %d hardware threads\n\n", w, h, tile, frames, nthreads);
  std::printf("%-6s %-10s %7s %9s %9s %9s %9s %8s\n", "scene", "mode", "threads", "ms/frame", "Mtri/s", "Mfrag/s",
              "earlyZ%", "speedup");
  for (const Case& c : cases) {
    struct Cfg {
      Mode mode;
      int threads;
    };
    std::vector<Cfg> cfgs = {{Mode::Immediate, 1}, {Mode::Tiled, 1}};
    if (nthreads > 1) cfgs.push_back({Mode::Tiled, nthreads});
    double base_ms = 0;
    for (const Cfg& cfg : cfgs) {
      const Run r = bench_scene(c.opt, w, h, cfg.mode, cfg.threads, tile, frames);
      if (base_ms == 0) base_ms = r.ms;
      const double s = r.ms / 1e3;
      const double covered = static_cast<double>(r.stats.raster.fragments_in);
      std::printf("%-6s %-10s %7d %9.2f %9.2f %9.1f %8.1f%% %7.2fx\n", c.name,
                  cfg.mode == Mode::Immediate ? "immediate" : "tiled", cfg.threads, r.ms,
                  static_cast<double>(r.stats.triangles_in) / s / 1e6,
                  static_cast<double>(r.stats.raster.fragments_shaded) / s / 1e6,
                  covered > 0 ? 100.0 * static_cast<double>(r.stats.raster.early_z_rejected) / covered : 0.0,
                  base_ms / r.ms);
    }
    std::printf("  triangles %llu, fragments shaded %llu/frame\n\n",
                static_cast<unsigned long long>(bench_scene(c.opt, w, h, Mode::Tiled, 1, tile, 1).stats.triangles_in),
                static_cast<unsigned long long>(bench_scene(c.opt, w, h, Mode::Tiled, 1, tile, 1).stats.raster.fragments_shaded));
  }
  return 0;
}
