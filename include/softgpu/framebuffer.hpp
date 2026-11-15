#pragma once
#include <vector>

#include "softgpu/image.hpp"
#include "softgpu/math.hpp"

namespace softgpu {

// RGBA8 color + float depth in [0, 1], 1 = far.
struct Framebuffer {
  int width = 0, height = 0;
  Image color;
  std::vector<float> depth;

  Framebuffer(int w, int h) : width(w), height(h), color(w, h), depth(static_cast<size_t>(w) * h, 1.0f) {}

  void clear(Vec4 c, float d = 1.0f) {
    const uint8_t r = to_u8(c.x), g = to_u8(c.y), b = to_u8(c.z), a = to_u8(c.w);
    for (size_t i = 0; i < color.rgba.size(); i += 4) {
      color.rgba[i] = r;
      color.rgba[i + 1] = g;
      color.rgba[i + 2] = b;
      color.rgba[i + 3] = a;
    }
    std::fill(depth.begin(), depth.end(), d);
  }
  float depth_at(int x, int y) const { return depth[static_cast<size_t>(y) * width + x]; }
};

}  // namespace softgpu
