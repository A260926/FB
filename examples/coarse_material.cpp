// Coarse material preview demo.
//
// Generates a procedural texture and three overlapping screen-filling
// triangle layers entirely in memory, then drives CoarseRenderer with real
// shader callbacks that perform nearest-neighbour texturing, simple
// directional lighting and per-block discards. The output is a binary P6
// PPM whose path is passed as the only command line argument.

#include "CoarseShading.h"
#include "Graphics3D.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <vector>

using namespace al::graphics;

namespace {

constexpr unsigned kWidth = 64u;
constexpr unsigned kHeight = 64u;

// Clip-space triangle covering the whole viewport. Its vertices sit on the
// left/bottom frustum edges and just past the right/top edges, so every clip
// plane has at most one vertex outside and the existing six-plane clipper
// never degenerates the primitive.
Triangle layerTriangle(double depth, float uvScale,
                       const Vector3f& normal)
{
  auto vertex = [&](double ndcX, double ndcY, float uCoord, float vCoord) {
    Vertex4f out;
    out.pos = Vector4f(static_cast<float>(ndcX * depth),
                       static_cast<float>(ndcY * depth),
                       0.0f,
                       static_cast<float>(depth));
    out.uv = Vector2f(uCoord, vCoord);
    out.rgb = Vector3f(1.0f, 1.0f, 1.0f);
    out.normal = normal; // intentionally left unnormalized
    return out;
  };
  return Triangle(
    vertex(-1.0, -1.0, 0.0f, 0.0f),
    vertex( 3.0, -1.0, uvScale, 0.0f),
    vertex(-1.0,  3.0, 0.0f, uvScale));
}

ImageData makeTexture()
{
  ImageData texture(8, 8);
  for (unsigned y = 0; y < 8; ++y) {
    for (unsigned x = 0; x < 8; ++x) {
      const bool checker = ((x / 2) + (y / 2)) % 2 == 0;
      const u8 r = checker ? 220 : 40;
      const u8 g = checker ? 60 : 200;
      const u8 b = checker ? 90 : 160;
      texture.setPixel(x, y, ImageData::makePixel(r, g, b, 255));
    }
  }
  return texture;
}

ShadingRateMap makeRateMap()
{
  const unsigned cellsX = (kWidth + 7u) / 8u;  // 8
  const unsigned cellsY = (kHeight + 7u) / 8u; // 8
  ShadingRateMap rates(static_cast<std::size_t>(cellsX) * cellsY);
  for (unsigned cy = 0; cy < cellsY; ++cy) {
    for (unsigned cx = 0; cx < cellsX; ++cx) {
      // Vertical bands so one frame contains every supported rate.
      const int rate = cx < 3 ? 1 : (cx < 6 ? 2 : 4);
      rates[cy * cellsX + cx] = rate;
    }
  }
  return rates;
}

bool writePpm(const char* path, const ImageData& image)
{
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "P6\n" << image.width() << ' ' << image.height() << "\n255\n";
  for (unsigned y = 0; y < image.height(); ++y) {
    for (unsigned x = 0; x < image.width(); ++x) {
      const u32 pixel = image.getPixel(x, y);
      const auto* rgba = reinterpret_cast<const std::uint8_t*>(&pixel);
      char rgb[3] = {static_cast<char>(rgba[0]),
                     static_cast<char>(rgba[1]),
                     static_cast<char>(rgba[2])};
      out.write(rgb, 3);
    }
  }
  return static_cast<bool>(out);
}

struct Layer
{
  Triangle triangle;
  unsigned calls = 0;
  // Returns a real coarse shader: textured, lit and selectively discarded.
  // parityMask == 0 means the layer never discards.
  CoarseShader makeShader(ImageData& texture, unsigned parityMask)
  {
    return [this, &texture, parityMask](
             const CoarseFragment& fragment, u32& colour) -> bool {
      ++calls;

      // Discard whole shading blocks on a repeating tile pattern so the
      // layers underneath become visible through the gaps.
      if (parityMask != 0u) {
        const unsigned tileX = static_cast<unsigned>(fragment.blockX) /
                               static_cast<unsigned>(fragment.rate);
        const unsigned tileY = static_cast<unsigned>(fragment.blockY) /
                               static_cast<unsigned>(fragment.rate);
        if (((tileX + tileY) & parityMask) == 0u) {
          return false;
        }
      }

      // Real nearest-neighbour texture lookup from interpolated UVs.
      const u32 texel = G3D::pixelAtUV(
        texture, fragment.uv.x(), fragment.uv.y());

      // Directional lighting from the (unnormalized) interpolated normal.
      const Vector3f& n = fragment.normal;
      const float length = std::sqrt(n.x() * n.x() +
                                     n.y() * n.y() +
                                     n.z() * n.z());
      float strength = 1.0f;
      if (length > 0.0f) {
        const Vector3f light(0.4f, -0.4f, 0.8246f);
        const float dot = (n.x() * light.x() + n.y() * light.y() +
                           n.z() * light.z()) / length;
        strength = std::max(0.5f, dot);
      }
      colour = G3D::lightPixel(texel, strength);
      return true;
    };
  }
};

} // namespace

int main(int argc, char** argv)
{
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s output.ppm\n", argv[0]);
    return 1;
  }

  ImageData target(kWidth, kHeight);
  ZBuffer depth(kWidth, kHeight);
  depth.fill(std::numeric_limits<double>::infinity());
  ImageData texture = makeTexture();

  CoarseRenderer renderer(target, depth);

  // Front to back: each layer uses a different depth, UV repeat and normal,
  // and a different discard pattern so deeper layers genuinely show through.
  Layer front{layerTriangle(2.0, 1.0f, Vector3f(0.0f, 0.0f, 2.0f))};
  Layer middle{layerTriangle(4.0, 2.0f, Vector3f(0.6f, 0.0f, 1.6f))};
  Layer back{layerTriangle(6.0, 3.0f, Vector3f(-0.6f, 0.6f, 1.4f))};

  renderer.submit(front.triangle, front.makeShader(texture, 1u));
  renderer.submit(middle.triangle, middle.makeShader(texture, 2u));
  renderer.submit(back.triangle, back.makeShader(texture, 0u)); // never discards

  const ShadingRateMap rates = makeRateMap();
  renderer.resolve(rates);

  if (!writePpm(argv[1], target)) {
    std::fprintf(stderr, "failed to write %s\n", argv[1]);
    return 1;
  }

  const std::size_t pixels = kWidth * kHeight;
  std::printf("coarse_material pixels=%zu shader_calls=%zu "
              "(front=%u middle=%u back=%u) rounds=%u committed=%zu "
              "candidates=%zu frontier=%zu\n",
              pixels, renderer.shader_calls,
              front.calls, middle.calls, back.calls,
              renderer.rounds, renderer.committed_pixels,
              renderer.captured_fragments, renderer.frontier_pixels);
  return 0;
}
