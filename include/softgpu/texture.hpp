#pragma once
#include <cstdint>
#include <vector>

#include "softgpu/math.hpp"

namespace softgpu {

enum class Filter { Nearest, Bilinear, Trilinear };
enum class Wrap { Repeat, Clamp };

// RGBA8 texture with a full box-filtered mip chain. Texel (0,0) is the top-left of level 0;
// u runs right, v runs down, both in [0,1) per repeat. Texel centres sit at (i + 0.5) / size,
// the convention every GPU uses for bilinear weights.
class Texture {
 public:
  struct Level {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
  };

  Texture() = default;
  Texture(int width, int height, std::vector<uint8_t> rgba);

  int levels() const { return static_cast<int>(levels_.size()); }
  const Level& level(int l) const { return levels_[l]; }
  int width() const { return levels_[0].width; }
  int height() const { return levels_[0].height; }

  Vec4 fetch(int level, int x, int y, Wrap wrap = Wrap::Repeat) const;
  Vec4 sample_nearest(int level, Vec2 uv, Wrap wrap = Wrap::Repeat) const;
  Vec4 sample_bilinear(int level, Vec2 uv, Wrap wrap = Wrap::Repeat) const;
  // Level of detail from the screen-space uv derivatives: log2 of the longer footprint axis in texels.
  float lod(Vec2 duv_dx, Vec2 duv_dy) const;
  // The GPU-style entry point: pick the level(s) from lod, then filter.
  Vec4 sample(Vec2 uv, float lod, Filter filter, Wrap wrap = Wrap::Repeat) const;
  Vec4 sample_grad(Vec2 uv, Vec2 duv_dx, Vec2 duv_dy, Filter filter, Wrap wrap = Wrap::Repeat) const {
    return sample(uv, lod(duv_dx, duv_dy), filter, wrap);
  }

  static Texture checkerboard(int width, int height, int cell, Vec4 a, Vec4 b);
  // A checkerboard with a different hue per mip level, to visualise which level a pixel used.
  static Texture mip_debug(int size);

 private:
  std::vector<Level> levels_;
};

}  // namespace softgpu
