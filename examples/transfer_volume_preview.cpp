// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

// Transfer-function CPU preview example.
//
// Builds a small volume entirely in memory (no geometry files), assigns a
// piecewise-linear transfer function, renders it through the transfer-volume
// path of SlTrace and writes a binary PPM image. Only the C++17 standard
// library is used besides the Fujiyama core headers and library.
//
// Build (after the core library has been built), from the repository root:
//   c++ -std=c++17 -O1 -I. -Isrc \
//       examples/transfer_volume_preview.cpp \
//       .task-build/libfujiyama.a -o transfer_volume_preview
// Run:
//   ./transfer_volume_preview preview.ppm

#include "fj_volume.h"
#include "fj_shading.h"
#include "fj_object_group.h"
#include "fj_object_instance.h"
#include "fj_volume_accelerator.h"
#include "fj_accelerator.h"
#include "fj_vector.h"
#include "fj_box.h"
#include "fj_color.h"
#include "fj_ray.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

using namespace fj;

namespace {

const int IMAGE_WIDTH = 96;
const int IMAGE_HEIGHT = 96;

// Deterministic smooth blob: a soft sphere with a narrow bright core, so the
// transfer-function preview shows both extinction and emission.
float procedural_density(int x, int y, int z, int resolution)
{
  const double fx = (x + .5) / resolution - .5;
  const double fy = (y + .5) / resolution - .5;
  const double fz = (z + .5) / resolution - .5;
  const double radius2 = fx * fx + fy * fy + fz * fz;

  const double shell = std::max(0., .35 - radius2);
  const double core = std::max(0., .05 - radius2) * 6.;
  const double value = shell * 1.4 + core;
  return static_cast<float>(value);
}

int to_byte(float value)
{
  if (value < 0) value = 0;
  if (value > 1) value = 1;
  // Gamma correction keeps the preview readable on a PPM viewer.
  return static_cast<int>(255 * std::pow(value, 1. / 2.2) + .5);
}

bool write_ppm(const char *path, const std::vector<Color4> &pixels,
    int width, int height)
{
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file << "P6\n" << width << " " << height << "\n255\n";
  for (const Color4 &pixel : pixels) {
    const uint8_t rgb[3] = {
        static_cast<uint8_t>(to_byte(pixel.r)),
        static_cast<uint8_t>(to_byte(pixel.g)),
        static_cast<uint8_t>(to_byte(pixel.b))};
    file.write(reinterpret_cast<const char *>(rgb), 3);
  }
  return static_cast<bool>(file);
}

} // anonymous namespace

int main(int argc, char **argv)
{
  const char *output_path = (argc > 1) ? argv[1] : "preview.ppm";

  const int resolution = 64;
  Volume volume;
  volume.Resize(resolution, resolution, resolution);
  volume.SetBounds(Box(Vector(-1, -1, -1), Vector(1, 1, 1)));

  for (int z = 0; z < resolution; z++) {
    for (int y = 0; y < resolution; y++) {
      for (int x = 0; x < resolution; x++) {
        volume.SetValue(x, y, z,
            procedural_density(x, y, z, resolution));
      }
    }
  }

  // Density 0 is empty; higher densities gain extinction and a warm emission.
  std::vector<TransferKnot> knots;
  knots.push_back(TransferKnot(0., 0., Color(0, 0, 0)));
  knots.push_back(TransferKnot(.15, .8, Color(.10f, .04f, .02f)));
  knots.push_back(TransferKnot(.45, 2.2, Color(.95f, .35f, .08f)));
  knots.push_back(TransferKnot(.8, 4.0, Color(1.f, .85f, .45f)));
  volume.SetTransferFunction(knots);

  ObjectInstance volume_object;
  if (volume_object.SetVolume(&volume) != 0) {
    return 2;
  }
  volume_object.ComputeBounds();

  ObjectGroup group;
  group.AddObject(&volume_object);
  group.ComputeBounds();
  const_cast<Accelerator *>(group.GetSurfaceAccelerator())->Build();
  if (VolumeAccBuild(const_cast<VolumeAccelerator *>(
          group.GetVolumeAccelerator())) != 0) {
    return 3;
  }

  TraceContext context = SlCameraContext(&group);
  context.transfer_volume = true;   // enable the transfer preview path
  context.raymarch_step = .03;
  context.raymarch_shadow_step = .03;

  std::vector<Color4> pixels(static_cast<size_t>(IMAGE_WIDTH) * IMAGE_HEIGHT);

  // Simple orthographic camera looking along +z through the unit volume.
  const double view_extent = 1.3;
  for (int row = 0; row < IMAGE_HEIGHT; row++) {
    for (int col = 0; col < IMAGE_WIDTH; col++) {
      const double u = (2. * col / (IMAGE_WIDTH - 1) - 1) * view_extent;
      const double v = (2. * row / (IMAGE_HEIGHT - 1) - 1) * view_extent;

      Ray ray;
      ray.orig = Vector(u, -v, -3);
      ray.dir = Vector(0, 0, 1);
      ray.tmin = .001;
      ray.tmax = 8;

      Color4 color;
      double t_hit = 0;
      SlTrace(&context, &ray.orig, &ray.dir, ray.tmin, ray.tmax,
          &color, &t_hit);
      pixels[static_cast<size_t>(row) * IMAGE_WIDTH + col] = color;
    }
  }

  if (!write_ppm(output_path, pixels, IMAGE_WIDTH, IMAGE_HEIGHT)) {
    std::fprintf(stderr, "failed to open %s for writing\n", output_path);
    return 4;
  }

  std::printf("wrote %s (%dx%d PPM)\n", output_path, IMAGE_WIDTH, IMAGE_HEIGHT);
  return 0;
}
