// Renders one of the demo scenes to a PPM/PNG, with the debugging switches a driver engineer
// reaches for first: stats, wireframe, tile heat map, depth dump.
#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>

#include "softgpu/args.hpp"
#include "softgpu/scenes.hpp"

using namespace softgpu;

namespace {

void usage() {
  std::printf(
      "usage: softgpu_render [--scene cube|torus|sphere|depth|blend] [--width W] [--height H]\n"
      "                      [--mode tiled|immediate] [--threads N] [--tile 32] [--out out.ppm|out.png]\n"
      "                      [--frames K] [--angle rad] [--detail N] [--count N] [--filter nearest|bilinear|trilinear]\n"
      "                      [--stats] [--wireframe] [--show-tiles] [--dump-stage depth]\n");
}

}  // namespace

int main(int argc, char** argv) {
  Args args(argc, argv);
  if (args.has("help")) {
    usage();
    return 0;
  }
  Config cfg;
  cfg.mode = args.str("mode", "tiled") == "immediate" ? Mode::Immediate : Mode::Tiled;
  cfg.threads = static_cast<int>(args.num("threads", static_cast<long>(std::thread::hardware_concurrency())));
  cfg.tile_size = static_cast<int>(args.num("tile", 32));
  const int width = static_cast<int>(args.num("width", 1280)), height = static_cast<int>(args.num("height", 720));
  const int frames = static_cast<int>(args.num("frames", 1));
  const std::string out = args.str("out", "out.ppm");

  SceneOptions opt;
  opt.name = args.str("scene", "cube");
  opt.angle = static_cast<float>(args.real("angle", 0.6));
  opt.detail = static_cast<int>(args.num("detail", 1));
  opt.count = static_cast<int>(args.num("count", 2000));
  const std::string filter = args.str("filter", "trilinear");
  opt.filter = filter == "nearest" ? Filter::Nearest : filter == "bilinear" ? Filter::Bilinear : Filter::Trilinear;

  Renderer r(width, height, cfg);
  r.set_wireframe(args.has("wireframe"));
  r.set_show_tiles(args.has("show-tiles"));

  for (int f = 0; f < frames; ++f) {
    r.stats().reset();
    if (frames > 1) opt.angle = static_cast<float>(args.real("angle", 0.0)) + 2 * kPi * static_cast<float>(f) / frames;
    if (!render_scene(r, opt)) {
      std::fprintf(stderr, "unknown scene '%s'\n", opt.name.c_str());
      usage();
      return 1;
    }
    std::string path = out;
    if (frames > 1) {
      char buf[16];
      std::snprintf(buf, sizeof buf, "_%03d", f);
      const size_t dot = path.rfind('.');
      path.insert(dot == std::string::npos ? path.size() : dot, buf);
    }
    if (!write_image(path, r.fb().color)) {
      std::fprintf(stderr, "cannot write %s\n", path.c_str());
      return 1;
    }
    if (args.str("dump-stage", "") == "depth") {
      const size_t dot = path.rfind('.');
      const std::string dpath = path.substr(0, dot == std::string::npos ? path.size() : dot) + ".depth.pgm";
      // Stretch the occupied depth range so the nonlinear z buffer is readable: near = dark.
      float lo = 1.0f, hi = 0.0f;
      for (float d : r.fb().depth)
        if (d < 1.0f) {
          lo = std::min(lo, d);
          hi = std::max(hi, d);
        }
      write_pgm(dpath, width, height, r.fb().depth.data(), lo, hi > lo ? hi : 1.0f);
      std::printf("wrote %s (depth range %.4f..%.4f)\n", dpath.c_str(), lo, hi);
    }
    std::printf("wrote %s (%dx%d, %s, %d thread%s, %.2f ms)\n", path.c_str(), width, height,
                cfg.mode == Mode::Tiled ? "tiled" : "immediate", cfg.threads, cfg.threads == 1 ? "" : "s",
                r.stats().total_ns / 1e6);
    if (args.has("stats")) r.stats().print(stdout);
  }
  return 0;
}
