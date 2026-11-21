//
// Compound lens rendering example.
//
// Builds a two-layer emissive grid scene, configures the CPU camera with a
// compound spherical lens, mechanically focuses it, renders with the production
// path tracer, and writes three outputs into the given directory:
//
//   irradiance.pfm  linear float radiance image
//   weight.pfm      mean exposure weight per camera sample (geometric factor)
//   valid.pgm       fraction of successful camera samples
//
// Usage: compound_lens_example <output-directory>
//

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "yocto_image.h"
#include "yocto_lens.h"
#include "yocto_scene.h"
#include "yocto_sceneio.h"
#include "yocto_trace.h"

using namespace yocto;

namespace {

// Simple linear PFM writer.
bool write_pfm(const std::string& filename, const image_data& image) {
  auto fs = std::fopen(filename.c_str(), "wb");
  if (!fs) return false;
  // PFM stores rows bottom-to-top and always three channels.
  auto header = std::string{"PF\n"} + std::to_string(image.width) + " " +
                std::to_string(image.height) + "\n-1.0\n";
  if (std::fwrite(header.data(), 1, header.size(), fs) != header.size()) {
    std::fclose(fs);
    return false;
  }
  auto ok = true;
  for (auto j = image.height - 1; j >= 0 && ok; j--) {
    for (auto i = 0; i < image.width; i++) {
      auto& pixel = image.pixels[(size_t)j * image.width + i];
      auto rgb   = std::array<float, 3>{pixel.x, pixel.y, pixel.z};
      if (std::fwrite(rgb.data(), sizeof(float), 3, fs) != 3) ok = false;
    }
  }
  std::fclose(fs);
  return ok;
}

// Simple binary PGM writer for a single channel image. PGM rows are stored
// top-to-bottom.
bool write_pgm(const std::string& filename, const image_data& image) {
  auto fs = std::fopen(filename.c_str(), "wb");
  if (!fs) return false;
  auto header = std::string{"P5\n"} + std::to_string(image.width) + " " +
                std::to_string(image.height) + "\n255\n";
  if (std::fwrite(header.data(), 1, header.size(), fs) != header.size()) {
    std::fclose(fs);
    return false;
  }
  auto ok = true;
  for (auto j = 0; j < image.height && ok; j++) {
    for (auto i = 0; i < image.width; i++) {
      auto value = image.pixels[(size_t)j * image.width + i].x;
      auto byte  = (unsigned char)(clamp(value, 0.0f, 1.0f) * 255.0f);
      if (std::fwrite(&byte, 1, 1, fs) != 1) ok = false;
    }
  }
  std::fclose(fs);
  return ok;
}

// Makes an emissive grid of quads on the z = 0 plane of its own instance.
shape_data make_grid_mesh(int nx, int ny, float size, float z, int material) {
  auto shape      = shape_data{};
  auto half       = size / 2;
  auto cell       = size / (float)nx;
  for (auto y = 0; y < ny; y++) {
    for (auto x = 0; x < nx; x++) {
      auto x0 = -half + x * cell;
      auto y0 = -half + y * cell;
      auto base = (int)shape.positions.size();
      shape.positions.push_back({x0, y0, z});
      shape.positions.push_back({x0 + cell, y0, z});
      shape.positions.push_back({x0 + cell, y0 + cell, z});
      shape.positions.push_back({x0, y0 + cell, z});
      shape.quads.push_back({base, base + 1, base + 2, base + 3});
    }
  }
  return shape;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
    return 1;
  }
  auto outdir = std::string{argv[1]};

  // Scene with a camera.
  auto scene = scene_data{};
  scene.cameras.push_back({});
  auto& camera  = scene.cameras[0];
  camera.aspect = 1.5f;
  camera.film   = 0.036f;
  // The camera sits at the origin looking down -z, so the lens +z maps to the
  // scene direction.
  camera.frame = lookat_frame({0, 0, 0.25f}, {0, 0, 0}, {0, 1, 0});

  // Front emitter (blue) and a back emitter (warm) seen through it.
  auto front_material    = material_data{};
  front_material.emission = {0.4f, 0.7f, 2.0f};
  scene.materials.push_back(front_material);
  auto back_material     = material_data{};
  back_material.emission = {3.0f, 1.6f, 0.6f};
  scene.materials.push_back(back_material);

  auto front_shape = make_grid_mesh(4, 4, 0.6f, 0.0f, 0);
  scene.shapes.push_back(front_shape);
  auto back_shape = make_grid_mesh(6, 6, 1.2f, 0.0f, 1);
  scene.shapes.push_back(back_shape);

  auto front_instance     = instance_data{};
  front_instance.shape    = 0;
  front_instance.material = 0;
  front_instance.frame.o  = {0, 0, -0.5f};
  scene.instances.push_back(front_instance);
  auto back_instance     = instance_data{};
  back_instance.shape    = 1;
  back_instance.material = 1;
  back_instance.frame.o  = {0, 0, -1.1f};
  scene.instances.push_back(back_instance);

  // Compound lens: a biconvex element behind a flat diaphragm.
  auto lens          = lens_config{};
  lens.aperture      = 0.012f;
  lens.film_distance = 0.05f;
  lens.interfaces.push_back({0.008f, 0.020f, 0.019f, 1.5f});
  lens.interfaces.push_back({0.022f, -0.020f, 0.019f, 1.0f});
  set_camera_lens(camera, lens);

  // Mechanically focus on the front emitter plane. Lens-local z points toward
  // the scene, so world z = -0.5 seen from a camera at z = 0.25 is at lens z =
  // 0.75.
  auto focus_error = std::string{};
  auto focus_z     = 0.75f;
  if (!focus_camera(camera, focus_z, 0.005f, 0.2f, &focus_error)) {
    std::fprintf(stderr, "focus failed: %s\n", focus_error.c_str());
    return 1;
  }

  // Validate before rendering.
  auto error = std::string{};
  if (!validate_camera_lens(camera, error)) {
    std::fprintf(stderr, "invalid lens: %s\n", error.c_str());
    return 1;
  }

  auto params     = trace_params{};
  params.resolution = 96;
  params.samples    = 32;
  params.bounces    = 3;
  params.sampler    = trace_sampler_type::path;
  params.clamp      = 1e30f;  // disable clamping for irradiance comparison
  params.seed       = 42;

  auto bvh    = make_trace_bvh(scene, params);
  auto lights = make_trace_lights(scene, params);
  auto state  = make_trace_state(scene, params);
  for (auto i = 0; i < params.samples; i++)
    trace_samples(state, scene, bvh, lights, params);

  auto irradiance = get_rendered_image(state);
  auto weight     = get_weight_image(state);
  auto valid      = get_valid_image(state);

  auto irradiance_path = outdir + "/irradiance.pfm";
  auto weight_path     = outdir + "/weight.pfm";
  auto valid_path      = outdir + "/valid.pgm";
  if (!write_pfm(irradiance_path, irradiance)) {
    std::fprintf(stderr, "cannot write %s\n", irradiance_path.c_str());
    return 1;
  }
  if (!write_pfm(weight_path, weight)) {
    std::fprintf(stderr, "cannot write %s\n", weight_path.c_str());
    return 1;
  }
  if (!write_pgm(valid_path, valid)) {
    std::fprintf(stderr, "cannot write %s\n", valid_path.c_str());
    return 1;
  }

  std::printf("compound_lens: wrote %s, %s, %s\n", irradiance_path.c_str(),
      weight_path.c_str(), valid_path.c_str());
  return 0;
}
