#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "softgpu/math.hpp"

namespace softgpu {

// 8-bit RGBA, row-major, y down.
struct Image {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;

  Image() = default;
  Image(int w, int h) : width(w), height(h), rgba(static_cast<size_t>(w) * h * 4, 0) {}
  uint8_t* at(int x, int y) { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
  const uint8_t* at(int x, int y) const { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
  void set(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    uint8_t* p = at(x, y);
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
  }
};

inline uint8_t to_u8(float v) { return static_cast<uint8_t>(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// Binary PPM (P6), alpha dropped.
bool write_ppm(const std::string& path, const Image& img);
// Binary PGM (P5) of a float buffer mapped [lo, hi] -> [0, 255].
bool write_pgm(const std::string& path, int w, int h, const float* v, float lo, float hi);
// RGBA PNG with stored (uncompressed) deflate blocks: valid everywhere, zero dependencies, not small.
bool write_png(const std::string& path, const Image& img);
// Picks by extension: .png -> PNG, anything else -> PPM.
bool write_image(const std::string& path, const Image& img);
// Reads a P6 PPM back (for tests and tools).
bool read_ppm(const std::string& path, Image& img);

}  // namespace softgpu
