/*
The MIT License (MIT)

Copyright (c) 2020 Light Transport Entertainment, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

//
// Standalone uv_baker example.
//
// It generates its own geometry (a floor with an occluding plate and a
// concave corner), its own UV layout and a deterministic cosine-weighted
// hemisphere direction set.  No model download, no window, no external
// texture.  Outputs ao.ppm (visibility) and bent.ppm (world bent normal
// mapped to RGB).
//

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "baker.h"

namespace {

// Deterministic radical inverse (base 2), used for stratified cosine sampling.
double RadicalInverse2(unsigned int i) {
  i = (i << 16u) | (i >> 16u);
  i = ((i & 0x55555555u) << 1u) | ((i & 0xAAAAAAAAu) >> 1u);
  i = ((i & 0x33333333u) << 2u) | ((i & 0xCCCCCCCCu) >> 2u);
  i = ((i & 0x0F0F0F0Fu) << 4u) | ((i & 0xF0F0F0F0u) >> 4u);
  i = ((i & 0x00FF00FFu) << 8u) | ((i & 0xFF00FF00u) >> 8u);
  return static_cast<double>(i) * 2.3283064365386963e-10;
}

// Deterministic cosine-weighted directions in the local +Z hemisphere.
// All are exact unit vectors and z >= 0.
std::vector<float> MakeCosineDirections(unsigned int count) {
  std::vector<float> dirs;
  dirs.reserve(3 * count);
  for (unsigned int i = 0; i < count; ++i) {
    const double u1 = (i + 0.5) / static_cast<double>(count);
    const double u2 = RadicalInverse2(i);
    const double r = std::sqrt(u1);
    const double phi = 2.0 * 3.14159265358979323846 * u2;
    const double x = r * std::cos(phi);
    const double y = r * std::sin(phi);
    const double z = std::sqrt(std::max(0.0, 1.0 - u1));
    const double len = std::sqrt(x * x + y * y + z * z);
    dirs.push_back(static_cast<float>(x / len));
    dirs.push_back(static_cast<float>(y / len));
    dirs.push_back(static_cast<float>(z / len));
  }
  return dirs;
}

struct Geometry {
  std::vector<float> positions;
  std::vector<unsigned int> indices;
  std::vector<float> uvs;
  std::vector<int> charts;
};

// A U-shaped cross section extruded into a floor / back wall / side wall with
// a separate floating plate above the floor.  UVs are laid out as three
// separate chart rectangles so the floor is partially occluded, partially
// open, and the corner is self-occluded.
Geometry MakeScene() {
  Geometry g;

  auto add_quad = [&g](const float p[4][3], const float uv[4][2], int chart) {
    const unsigned int base = static_cast<unsigned int>(g.positions.size() / 3);
    for (int i = 0; i < 4; ++i) {
      g.positions.push_back(p[i][0]);
      g.positions.push_back(p[i][1]);
      g.positions.push_back(p[i][2]);
    }
    // Two CCW triangles (0,1,2) and (0,2,3).
    const unsigned int tri[2][3] = {{base + 0, base + 1, base + 2},
                                    {base + 0, base + 2, base + 3}};
    for (int t = 0; t < 2; ++t) {
      for (int c = 0; c < 3; ++c) {
        g.indices.push_back(tri[t][c]);
      }
      for (int c = 0; c < 3; ++c) {
        const int corner = static_cast<int>(tri[t][c] - base);
        g.uvs.push_back(uv[corner][0]);
        g.uvs.push_back(uv[corner][1]);
      }
      g.charts.push_back(chart);
    }
  };

  // Floor: z = 0, facing +Z.  UV chart [0, 0.5] x [0, 0.5].
  {
    const float p[4][3] = {{-2.0f, -2.0f, 0.0f},
                           {2.0f, -2.0f, 0.0f},
                           {2.0f, 2.0f, 0.0f},
                           {-2.0f, 2.0f, 0.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.5f, 0.5f}, {0.0f, 0.5f}};
    add_quad(p, uv, 0);
  }

  // Back wall: y = 2, facing -Y.  UV chart [0.5, 1] x [0, 0.5].
  {
    const float p[4][3] = {{-2.0f, 2.0f, 0.0f},
                           {2.0f, 2.0f, 0.0f},
                           {2.0f, 2.0f, 2.0f},
                           {-2.0f, 2.0f, 2.0f}};
    const float uv[4][2] = {{0.5f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.5f}, {0.5f, 0.5f}};
    add_quad(p, uv, 1);
  }

  // Left wall: x = -2, facing +X.  UV chart [0, 0.5] x [0.5, 1].
  {
    const float p[4][3] = {{-2.0f, -2.0f, 0.0f},
                           {-2.0f, 2.0f, 0.0f},
                           {-2.0f, 2.0f, 2.0f},
                           {-2.0f, -2.0f, 2.0f}};
    const float uv[4][2] = {{0.0f, 0.5f}, {0.5f, 0.5f}, {0.5f, 1.0f}, {0.0f, 1.0f}};
    add_quad(p, uv, 2);
  }

  // Near occluding plate hovering at z = 1 over the floor center, facing -Z
  // so it is a one-sided occluder relative to the floor's +Z hemisphere.
  {
    const float p[4][3] = {{-0.75f, -0.75f, 1.0f},
                           {0.75f, -0.75f, 1.0f},
                           {0.75f, 0.75f, 1.0f},
                           {-0.75f, 0.75f, 1.0f}};
    const float uv[4][2] = {{0.5f, 0.5f}, {0.75f, 0.5f}, {0.75f, 0.75f},
                            {0.5f, 0.75f}};
    add_quad(p, uv, 3);
  }

  return g;
}

bool WritePPM(const std::string &filename, int width, int height,
              const std::vector<unsigned char> &rgb) {
  FILE *fp = std::fopen(filename.c_str(), "wb");
  if (!fp) return false;
  std::fprintf(fp, "P6\n%d %d\n255\n", width, height);
  const std::size_t written =
      std::fwrite(rgb.data(), 1, rgb.size(), fp);
  std::fclose(fp);
  return written == rgb.size();
}

}  // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  const int width = 64;
  const int height = 64;

  const Geometry g = MakeScene();
  const std::vector<float> directions = MakeCosineDirections(64);

  uv_baker::Input input;
  input.positions = g.positions.data();
  input.position_count = g.positions.size();
  input.triangle_indices = g.indices.data();
  input.triangle_index_count = g.indices.size();
  input.face_uvs = g.uvs.data();
  input.face_uv_count = g.uvs.size();
  input.face_chart_ids = g.charts.data();
  input.face_chart_id_count = g.charts.size();
  input.directions = directions.data();
  input.direction_count = directions.size();

  uv_baker::Options options;
  options.width = width;
  options.height = height;
  options.radius = 1.5f;
  options.bias = 1.0e-3f;
  options.dilation_radius = 1;

  uv_baker::Result result;
  std::string error;
  if (!uv_baker::Bake(input, options, &result, &error)) {
    std::fprintf(stderr, "bake failed: %s\n", error.c_str());
    return EXIT_FAILURE;
  }

  std::vector<unsigned char> ao(3 * static_cast<std::size_t>(width) * height);
  std::vector<unsigned char> bent(3 * static_cast<std::size_t>(width) * height);

  std::size_t occluded = 0;
  std::size_t unoccluded = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      // PPM rows are stored top to bottom; the result is bottom to top.
      const std::size_t dst = static_cast<std::size_t>(y) *
                                  static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x);
      const std::size_t src = static_cast<std::size_t>(height - 1 - y) *
                                  static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x);

      const float visibility = result.has_value[src] ? result.visibility[src] : 0.0f;
      const unsigned char v =
          static_cast<unsigned char>(255.0f * visibility + 0.5f);
      ao[3 * dst + 0] = v;
      ao[3 * dst + 1] = v;
      ao[3 * dst + 2] = v;

      for (std::size_t c = 0; c < 3; ++c) {
        const float n = result.has_value[src]
                            ? result.bent_normals[3 * src + c]
                            : 0.0f;
        const float mapped = 0.5f * n + 0.5f;
        bent[3 * dst + c] = static_cast<unsigned char>(
            255.0f * std::min(1.0f, std::max(0.0f, mapped)) + 0.5f);
      }

      if (result.real_coverage[src]) {
        if (visibility < 1.0f) {
          ++occluded;
        } else {
          ++unoccluded;
        }
      }
    }
  }

  if (!WritePPM("ao.ppm", width, height, ao) ||
      !WritePPM("bent.ppm", width, height, bent)) {
    std::fprintf(stderr, "failed to write PPM output\n");
    return EXIT_FAILURE;
  }

  std::printf("covered=%zu occluded=%zu unoccluded=%zu\n",
              static_cast<std::size_t>(occluded + unoccluded), occluded,
              unoccluded);
  std::printf("wrote ao.ppm and bent.ppm (%dx%d)\n", width, height);
  return EXIT_SUCCESS;
}
