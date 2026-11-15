#ifndef ANDREW_LIM_GRAPHICS_COARSE_SHADING_H
#define ANDREW_LIM_GRAPHICS_COARSE_SHADING_H

#include "ImageData.h"
#include "Triangle.h"
#include "TriangleDrawer.h"
#include "Vector2f.h"
#include "Vector3f.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace al { namespace graphics {

// One shading rate entry per 8x8 pixel cell of the target, stored row by
// row. Every value must be 1, 2 or 4 and the map must hold
// ceil(width/8) * ceil(height/8) entries.
using ShadingRateMap = std::vector<int>;

// Everything a coarse shader invocation needs. A single invocation shades
// every pixel listed in coverageMask for one original primitive inside one
// r*r shading block.
struct CoarseFragment
{
  // Submission index of the original primitive (0 based).
  std::size_t primitive;
  // 0 based resolve round.
  unsigned int round;
  // Top left corner of the r*r shading block in target pixels.
  int blockX;
  int blockY;
  // Shading rate of the 8x8 cell that owns this block (1, 2 or 4).
  int rate;
  // Bit j*rate+i is set when block pixel (i, j) selected this primitive
  // during the current round.
  std::uint32_t coverageMask;
  // Arithmetic mean of the covered pixel centres, in target pixels.
  double sampleX;
  double sampleY;
  // Perspective reconstructed w at the representative position.
  double depth;
  // Perspective correct attributes evaluated at (sampleX, sampleY).
  Vector2f uv;
  // Screen-space UV derivatives for a single pixel, independent of rate.
  Vector2f duvdx;
  Vector2f duvdy;
  Vector3f rgb;
  // Interpolated vertex normal, intentionally not normalized.
  Vector3f normal;
};

// Return true with a non-zero alpha colour to commit the block, false (or
// alpha 0) to discard the current candidates so deeper surfaces can show.
using CoarseShader =
  std::function<bool(const CoarseFragment& fragment, u32& colour)>;

/*
Coarse material preview renderer.

Submitted clip-space triangles are kept untouched until resolve(). Each
resolve runs the existing six-plane clipping and the shared BARYCENTRIC
coverage traversal exactly once per primitive, collecting per-pixel
candidates. Shading then advances one frontier layer at a time and groups
the frontier pixels by original primitive and r*r shading block, so a
shader only runs once per group while occlusion stays per pixel.
*/
class CoarseRenderer
{
public:
  // The target and depth dimensions are fixed for the renderer's lifetime.
  CoarseRenderer(ImageData& target, ZBuffer& depth);

  // Stores the clip-space triangle (before perspective division) and the
  // shader callback by value. Position components must be finite and every
  // w must be strictly positive; otherwise the call is rejected and does
  // not consume a submission index. Nothing is shaded or written here.
  // Any data borrowed by the callback (e.g. captured references) must stay
  // alive on the caller's side until resolve() returns.
  void submit(const Triangle& clipTriangle, const CoarseShader& shader);

  // Validates the rate map and the target/depth dimensions before any
  // callback runs or any pixel is written, throwing std::invalid_argument
  // and keeping the pending batch on failure. On success the batch is
  // cleared and the submission index is reset; the updated target/depth
  // become the baseline for the next batch.
  void resolve(const ShadingRateMap& rates);

  // Drops the pending batch without touching the target or depth.
  void reset();

  // Statistics from the most recent resolve().
  std::size_t captured_fragments;
  std::size_t rasterized_primitives;
  std::size_t frontier_pixels;
  std::size_t shader_calls;
  std::size_t committed_pixels;
  unsigned int rounds;

private:
  struct PendingPrimitive
  {
    Triangle triangle;
    CoarseShader shader;
    std::size_t index;
  };

  struct Candidate
  {
    std::size_t primitive;
    double depth;
  };

  struct Group
  {
    std::size_t primitive;
    int blockX;
    int blockY;
    int rate;
    std::uint32_t coverageMask;
    // Pixels belonging to this round's group, parallel arrays.
    std::vector<int> pixelX;
    std::vector<int> pixelY;
    std::vector<double> pixelDepth;
  };

  ImageData& target_;
  ZBuffer& depth_;
  unsigned int targetWidth_;
  unsigned int targetHeight_;
  std::vector<PendingPrimitive> pending_;
  std::size_t nextSubmission_;

  static bool finiteTriangle(const Triangle& triangle);
  static void interpolateAttributes(const PendingPrimitive& primitive,
                                    int width, int height,
                                    double sampleX, double sampleY,
                                    CoarseFragment& fragment);
};

} // namespace graphics
} // namespace al

#endif
