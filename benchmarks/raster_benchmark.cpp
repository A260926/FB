#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;

namespace {
struct BenchmarkSettings {
  std::string obj =
      std::string(BENCHMARK_ASSET_DIR) + "/spot/spot_triangulated_good.obj";
  std::string output = "benchmark-results/raster", mode = "both";
  std::size_t width = 1024, height = 1024, warmup = 100, frames = 1000;
  bool wireframe = false;
};

std::size_t positiveInteger(const std::string &value) {
  if (value.empty() ||
      value.find_first_not_of("0123456789") != std::string::npos) {
    throw std::invalid_argument("expected a positive integer: " + value);
  }
  auto number = std::stoull(value);
  if (!number || number > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("integer is outside the supported range");
  }
  return static_cast<std::size_t>(number);
}

BenchmarkSettings parseSettings(int argc, char **argv) {
  BenchmarkSettings settings;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto value = [&]() {
      if (i + 1 >= argc) {
        throw std::invalid_argument("missing value for " + argument);
      }
      return std::string(argv[++i]);
    };
    if (argument == "--obj") {
      settings.obj = value();
    } else if (argument == "--output") {
      settings.output = value();
    } else if (argument == "--width") {
      settings.width = positiveInteger(value());
    } else if (argument == "--height") {
      settings.height = positiveInteger(value());
    } else if (argument == "--warmup") {
      settings.warmup = positiveInteger(value());
    } else if (argument == "--frames") {
      settings.frames = positiveInteger(value());
    } else if (argument == "--mode") {
      settings.mode = value();
    } else if (argument == "--wireframe") {
      settings.wireframe = true;
    } else {
      throw std::invalid_argument("unknown argument: " + argument);
    }
  }
  if (settings.mode != "both" && settings.mode != "scalar" &&
      settings.mode != "simd") {
    throw std::invalid_argument("mode must be both, scalar or simd");
  }
  return settings;
}

std::shared_ptr<Scene> createBenchmarkScene(const BenchmarkSettings &settings,
                                            std::size_t &triangleCount) {
  auto mesh = ObjLoader(settings.obj).load();
  const auto bounds = mesh->getBounds();
  if (bounds.empty()) {
    throw std::invalid_argument("OBJ has no geometry");
  }
  triangleCount = mesh->triangleCount();
  const auto center = bounds.centroid();
  const float size = std::max(.01f, maxComponent(bounds.diagonal()));
  auto scene = std::make_shared<Scene>();
  scene->addGraphicObj(mesh, "model");
  // Same automatic framing and point light as the OBJ CLI path.
  scene->setViewMatrix(center + glm::vec3(0, 0, 2 * size), center, {0, 1, 0});
  scene->setProjectionMatrix(45, size * .001f, size * 20);
  scene->addLight("key", std::make_shared<light_struct>(
                             center + glm::vec3(size, size, size),
                             glm::vec3(8 * size * size)));
  scene->prepare(); // Loading / initial acceleration build is outside timing.
  return scene;
}

struct FrameAgreement {
  std::size_t coveredPixels = 0;
  float colorError = 0, depthError = 0;
};

FrameAgreement checkFrames(const TraditionalRasterizer &scalar,
                           const TraditionalRasterizer &simd) {
  FrameAgreement agreement;
  for (std::size_t pixel = 0; pixel < scalar.pixels().size(); ++pixel) {
    const bool scalarCovered = std::isfinite(scalar.depth()[pixel]);
    const bool simdCovered = std::isfinite(simd.depth()[pixel]);
    if (scalarCovered != simdCovered || !finite(scalar.pixels()[pixel]) ||
        !finite(simd.pixels()[pixel])) {
      throw std::runtime_error(
          "scalar/SIMD coverage or finite-color check failed");
    }
    agreement.colorError = std::max(
        agreement.colorError,
        maxComponent(glm::abs(scalar.pixels()[pixel] - simd.pixels()[pixel])));
    if (scalarCovered) {
      ++agreement.coveredPixels;
      agreement.depthError =
          std::max(agreement.depthError,
                   std::abs(scalar.depth()[pixel] - simd.depth()[pixel]));
    }
  }
  if (!agreement.coveredPixels || agreement.colorError > 2e-5f ||
      agreement.depthError > 1e-6f) {
    throw std::runtime_error("scalar/SIMD framebuffer agreement failed");
  }
  return agreement;
}

// Linear interpolation at rank q*(N-1), also called Hyndman-Fan type 7.
double percentile(const std::vector<double> &sorted, double q) {
  const double rank = q * (sorted.size() - 1);
  const auto low = static_cast<std::size_t>(rank);
  const auto high = std::min(low + 1, sorted.size() - 1);
  return sorted[low] + (sorted[high] - sorted[low]) * (rank - low);
}

std::string jsonString(const std::string &value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') {
      result += '\\';
    }
    if (c < 32) {
      throw std::invalid_argument("control character in output metadata");
    }
    result += static_cast<char>(c);
  }
  return result + '"';
}

void writeSummary(std::ostream &output, const char *mode,
                  const std::vector<double> &samples) {
  auto sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  double total = 0;
  for (double value : samples) {
    total += value;
  }
  const double mean = total / samples.size();
  output << "    {\"mode\": " << jsonString(mode)
         << ", \"samples\": " << samples.size() << ", \"mean_ms\": " << mean
         << ", \"min_ms\": " << sorted.front()
         << ", \"p10_ms\": " << percentile(sorted, .10)
         << ", \"p50_ms\": " << percentile(sorted, .50)
         << ", \"p90_ms\": " << percentile(sorted, .90)
         << ", \"p95_ms\": " << percentile(sorted, .95)
         << ", \"p99_ms\": " << percentile(sorted, .99)
         << ", \"max_ms\": " << sorted.back()
         << ", \"draws_per_second\": " << 1000 / mean << "}";
  std::cout << mode << ": p50=" << percentile(sorted, .50)
            << " ms, p95=" << percentile(sorted, .95)
            << " ms, p99=" << percentile(sorted, .99) << " ms\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout
          << "raster_benchmark [--obj file.obj] [--width 1024 --height 1024]\n"
             "  [--warmup 100 --frames 1000] [--mode both|scalar|simd]\n"
             "  [--output benchmark-results/raster] [--wireframe]\n"
             "Times draw() only. Writes <output>.csv and <output>.json.\n";
      return 0;
    }
    const auto settings = parseSettings(argc, argv);
    const auto primitive =
        settings.wireframe ? Primitive::LINES : Primitive::TRIANGLES;
    std::size_t triangles = 0;
    auto scene = createBenchmarkScene(settings, triangles);
    TraditionalRasterizer scalar(settings.width, settings.height),
        simd(settings.width, settings.height);
    scalar.setSIMD(false);
    simd.setSIMD(true);
    scalar.addScene(scene);
    simd.addScene(scene);
    scalar.draw(primitive);
    simd.draw(primitive);
    const auto agreement = checkFrames(scalar, simd);
    const bool useScalar = settings.mode != "simd",
               useSIMD = settings.mode != "scalar";
    std::cout << "Loaded " << triangles << " triangles; "
              << agreement.coveredPixels
              << " covered pixels. Framebuffers agree. Warming up "
              << settings.warmup << " draws per enabled mode.\n"
              << std::flush;
    for (std::size_t frame = 0; frame < settings.warmup; ++frame) {
      if (frame % 2 == 0) {
        if (useScalar) {
          scalar.draw(primitive);
        }
        if (useSIMD) {
          simd.draw(primitive);
        }
      } else {
        if (useSIMD) {
          simd.draw(primitive);
        }
        if (useScalar) {
          scalar.draw(primitive);
        }
      }
    }
    std::vector<double> scalarTimes, simdTimes;
    scalarTimes.reserve(settings.frames);
    simdTimes.reserve(settings.frames);
    const auto outputParent =
        std::filesystem::path(settings.output).parent_path();
    if (!outputParent.empty()) {
      std::filesystem::create_directories(outputParent);
    }

    struct FrameTime {
      std::size_t frame;
      const char *mode;
      double ms;
    };

    std::vector<FrameTime> records;
    records.reserve(settings.frames * 2);
    auto measure = [&](TraditionalRasterizer &renderer,
                       std::vector<double> &times, const char *mode,
                       std::size_t frame) {
      const auto start = std::chrono::steady_clock::now();
      renderer.draw(primitive);
      const auto end = std::chrono::steady_clock::now();
      const double elapsed =
          std::chrono::duration<double, std::milli>(end - start).count();
      times.push_back(elapsed);
      records.push_back({frame, mode, elapsed});
    };
    // Alternate paired order to reduce systematic first/second-mode bias.
    // No file I/O, display, allocation of sample buffers or checks inside draw
    // timing.
    for (std::size_t frame = 0; frame < settings.frames; ++frame) {
      if (frame % 2 == 0) {
        if (useScalar) {
          measure(scalar, scalarTimes, "scalar", frame);
        }
        if (useSIMD) {
          measure(simd, simdTimes, "simd", frame);
        }
      } else {
        if (useSIMD) {
          measure(simd, simdTimes, "simd", frame);
        }
        if (useScalar) {
          measure(scalar, scalarTimes, "scalar", frame);
        }
      }
      if ((frame + 1) % 250 == 0) {
        std::cout << "Measured " << frame + 1 << '/' << settings.frames
                  << " frames per enabled mode\n"
                  << std::flush;
      }
    }
    checkFrames(scalar, simd);
    std::ofstream csv(settings.output + ".csv"),
        json(settings.output + ".json");
    if (!csv || !json) {
      throw std::runtime_error("cannot write benchmark results");
    }
    csv << std::setprecision(12) << "frame,mode,draw_ms\n";
    for (const auto &record : records) {
      csv << record.frame << ',' << record.mode << ',' << record.ms << '\n';
    }
    json
        << std::setprecision(12) << "{\n  \"obj\": "
        << jsonString(std::filesystem::absolute(settings.obj).string())
        << ",\n  \"width\": " << settings.width
        << ", \"height\": " << settings.height
        << ",\n  \"input_triangles\": " << triangles
        << ", \"covered_pixels\": " << agreement.coveredPixels
        << ",\n  \"warmup_per_mode\": " << settings.warmup
        << ", \"frames_per_mode\": " << settings.frames
        << ",\n  \"primitive\": "
        << jsonString(settings.wireframe ? "wireframe" : "filled")
        << ",\n  \"shader\": \"PHONG with OBJ/default materials and one point "
           "light\""
        << ",\n  \"compiler\": " << jsonString(BENCHMARK_COMPILER)
        << ",\n  \"build_type\": " << jsonString(BENCHMARK_BUILD_TYPE)
        << ",\n  \"tbb_enabled\": "
        << (SOFT_RASTERIZER_USE_TBB ? "true" : "false")
        << ",\n  \"timer\": \"steady_clock, wall-clock milliseconds\""
        << ",\n  \"scope\": \"draw(): clear, scene.prepare, projection, "
           "binning, rasterization, shading; excludes load, display, image "
           "encoding, I/O\""
        << ",\n  \"percentile\": \"linear interpolation at q*(N-1), type 7\""
        << ",\n  \"paired_order\": \"alternating scalar/SIMD order each frame\""
        << ",\n  \"max_color_error\": " << agreement.colorError
        << ", \"max_depth_error\": " << agreement.depthError
        << ",\n  \"results\": [\n";
    bool comma = false;
    if (useScalar) {
      writeSummary(json, "scalar", scalarTimes);
      comma = true;
    }
    if (useSIMD) {
      if (comma) {
        json << ",\n";
      }
      writeSummary(json, "simd", simdTimes);
    }
    json << "\n  ]\n}\n";
    if (!csv || !json) {
      throw std::runtime_error("failed to write benchmark results");
    }
    std::cout << "Saved " << settings.output << ".csv and .json\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
