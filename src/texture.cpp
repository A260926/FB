#include "softgpu/texture.hpp"

#include "softgpu/image.hpp"

#include <algorithm>
#include <cmath>

namespace softgpu {

namespace {

int wrap_coord(int i, int n, Wrap wrap) {
  if (wrap == Wrap::Clamp) return i < 0 ? 0 : (i >= n ? n - 1 : i);
  const int m = i % n;
  return m < 0 ? m + n : m;
}

}  // namespace

Texture::Texture(int width, int height, std::vector<uint8_t> rgba) {
  levels_.push_back({width, height, std::move(rgba)});
  while (levels_.back().width > 1 || levels_.back().height > 1) {
    const Level& src = levels_.back();
    Level dst;
    dst.width = std::max(1, src.width / 2);
    dst.height = std::max(1, src.height / 2);
    dst.rgba.resize(static_cast<size_t>(dst.width) * dst.height * 4);
    for (int y = 0; y < dst.height; ++y)
      for (int x = 0; x < dst.width; ++x)
        for (int c = 0; c < 4; ++c) {
          int sum = 0;
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
              const int sx = std::min(src.width - 1, x * 2 + dx), sy = std::min(src.height - 1, y * 2 + dy);
              sum += src.rgba[(static_cast<size_t>(sy) * src.width + sx) * 4 + c];
            }
          dst.rgba[(static_cast<size_t>(y) * dst.width + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
        }
    levels_.push_back(std::move(dst));
  }
}

Vec4 Texture::fetch(int level, int x, int y, Wrap wrap) const {
  const Level& l = levels_[level];
  x = wrap_coord(x, l.width, wrap);
  y = wrap_coord(y, l.height, wrap);
  const uint8_t* p = &l.rgba[(static_cast<size_t>(y) * l.width + x) * 4];
  const float s = 1.0f / 255.0f;
  return {p[0] * s, p[1] * s, p[2] * s, p[3] * s};
}

Vec4 Texture::sample_nearest(int level, Vec2 uv, Wrap wrap) const {
  const Level& l = levels_[level];
  return fetch(level, static_cast<int>(std::floor(uv.x * l.width)), static_cast<int>(std::floor(uv.y * l.height)),
               wrap);
}

Vec4 Texture::sample_bilinear(int level, Vec2 uv, Wrap wrap) const {
  const Level& l = levels_[level];
  const float fx = uv.x * l.width - 0.5f, fy = uv.y * l.height - 0.5f;
  const float x0f = std::floor(fx), y0f = std::floor(fy);
  const float tx = fx - x0f, ty = fy - y0f;
  const int x0 = static_cast<int>(x0f), y0 = static_cast<int>(y0f);
  const Vec4 c00 = fetch(level, x0, y0, wrap), c10 = fetch(level, x0 + 1, y0, wrap);
  const Vec4 c01 = fetch(level, x0, y0 + 1, wrap), c11 = fetch(level, x0 + 1, y0 + 1, wrap);
  const Vec4 top = c00 + (c10 - c00) * tx;
  const Vec4 bot = c01 + (c11 - c01) * tx;
  return top + (bot - top) * ty;
}

float Texture::lod(Vec2 duv_dx, Vec2 duv_dy) const {
  const float w = static_cast<float>(width()), h = static_cast<float>(height());
  const float dx2 = duv_dx.x * w * duv_dx.x * w + duv_dx.y * h * duv_dx.y * h;
  const float dy2 = duv_dy.x * w * duv_dy.x * w + duv_dy.y * h * duv_dy.y * h;
  const float rho2 = std::max(dx2, dy2);
  return rho2 > 0 ? 0.5f * std::log2(rho2) : -100.0f;
}

Vec4 Texture::sample(Vec2 uv, float lod, Filter filter, Wrap wrap) const {
  const int max_level = levels() - 1;
  lod = clampf(lod, 0.0f, static_cast<float>(max_level));
  switch (filter) {
    case Filter::Nearest: return sample_nearest(static_cast<int>(lod + 0.5f), uv, wrap);
    case Filter::Bilinear: return sample_bilinear(static_cast<int>(lod + 0.5f), uv, wrap);
    default: {
      const int l0 = static_cast<int>(lod);
      const int l1 = std::min(max_level, l0 + 1);
      const float t = lod - static_cast<float>(l0);
      const Vec4 a = sample_bilinear(l0, uv, wrap);
      if (t <= 0.0f || l1 == l0) return a;
      const Vec4 b = sample_bilinear(l1, uv, wrap);
      return a + (b - a) * t;
    }
  }
}

Texture Texture::checkerboard(int width, int height, int cell, Vec4 a, Vec4 b) {
  std::vector<uint8_t> px(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const Vec4& c = ((x / cell) + (y / cell)) % 2 ? b : a;
      uint8_t* p = &px[(static_cast<size_t>(y) * width + x) * 4];
      p[0] = to_u8(c.x);
      p[1] = to_u8(c.y);
      p[2] = to_u8(c.z);
      p[3] = to_u8(c.w);
    }
  return Texture(width, height, std::move(px));
}

Texture Texture::mip_debug(int size) {
  Texture t = checkerboard(size, size, std::max(1, size / 8), Vec4(1, 1, 1, 1), Vec4(0.2f, 0.2f, 0.2f, 1));
  const Vec3 hues[] = {{1, 0.3f, 0.3f}, {0.3f, 1, 0.3f}, {0.3f, 0.3f, 1}, {1, 1, 0.3f}, {1, 0.3f, 1}, {0.3f, 1, 1}};
  for (size_t l = 1; l < t.levels_.size(); ++l) {
    const Vec3 h = hues[(l - 1) % 6];
    for (size_t i = 0; i < t.levels_[l].rgba.size(); i += 4) {
      t.levels_[l].rgba[i] = to_u8(t.levels_[l].rgba[i] / 255.0f * h.x);
      t.levels_[l].rgba[i + 1] = to_u8(t.levels_[l].rgba[i + 1] / 255.0f * h.y);
      t.levels_[l].rgba[i + 2] = to_u8(t.levels_[l].rgba[i + 2] / 255.0f * h.z);
    }
  }
  return t;
}

}  // namespace softgpu
