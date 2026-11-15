#include "CoarseShading.h"
#include "ClipSpace.h"
#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

using namespace al::graphics;

namespace {

constexpr unsigned CW = 16u;
constexpr unsigned CH = 16u;

// Builds a clip-space vertex (before perspective division, w > 0) from the
// window position it should land on.
Vertex4f winVertexAt(double wx, double wy, double w,
                     unsigned width, unsigned height,
                     const Vector2f& uv = Vector2f(),
                     const Vector3f& rgb = Vector3f(),
                     const Vector3f& normal = Vector3f())
{
  const double ndcX = 2.0 * wx / width - 1.0;
  const double ndcY = 1.0 - 2.0 * wy / height;
  Vertex4f vertex;
  vertex.pos = Vector4f(static_cast<float>(ndcX * w),
                        static_cast<float>(ndcY * w),
                        0.0f,
                        static_cast<float>(w));
  vertex.uv = uv;
  vertex.rgb = rgb;
  vertex.normal = normal;
  return vertex;
}

Vertex4f winVertex(double wx, double wy, double w,
                   const Vector2f& uv = Vector2f(),
                   const Vector3f& rgb = Vector3f(),
                   const Vector3f& normal = Vector3f())
{
  return winVertexAt(wx, wy, w, CW, CH, uv, rgb, normal);
}

// Independent point-in-triangle test using the same CCW y-down convention
// as the rasterizer, so expected coverage can be computed in the tests
// without reusing production code.
bool insideTriangle(const double win[3][2], double px, double py)
{
  auto cross = [&](int a, int b) {
    return (win[b][0] - win[a][0]) * (py - win[a][1]) -
           (win[b][1] - win[a][1]) * (px - win[a][0]);
  };
  return cross(0, 1) <= 0.0 && cross(1, 2) <= 0.0 && cross(2, 0) <= 0.0;
}

std::vector<std::pair<int, int>>
coveredPixels(const double win[3][2], unsigned width, unsigned height)
{
  std::vector<std::pair<int, int>> pixels;
  for (unsigned y = 0; y < height; ++y) {
    for (unsigned x = 0; x < width; ++x) {
      if (insideTriangle(win, x + 0.5, y + 0.5)) {
        pixels.emplace_back(static_cast<int>(x), static_cast<int>(y));
      }
    }
  }
  return pixels;
}

void windowBary(const double win[3][2], double px, double py,
                double lambda[3])
{
  const double area =
    (win[1][0] - win[0][0]) * (win[2][1] - win[0][1]) -
    (win[1][1] - win[0][1]) * (win[2][0] - win[0][0]);
  auto cross = [&](int a, int b) {
    return (win[b][0] - win[a][0]) * (py - win[a][1]) -
           (win[b][1] - win[a][1]) * (px - win[a][0]);
  };
  lambda[0] = cross(1, 2) / area;
  lambda[1] = cross(2, 0) / area;
  lambda[2] = cross(0, 1) / area;
}

int popCount(std::uint32_t mask)
{
  int count = 0;
  while (mask) {
    count += static_cast<int>(mask & 1u);
    mask >>= 1;
  }
  return count;
}

int lowestBitIndex(std::uint32_t mask)
{
  int index = 0;
  while ((mask & 1u) == 0u) {
    mask >>= 1;
    ++index;
  }
  return index;
}

ShadingRateMap uniformRates(unsigned width, unsigned height, int rate)
{
  const unsigned cellsX = (width + 7u) / 8u;
  const unsigned cellsY = (height + 7u) / 8u;
  return ShadingRateMap(static_cast<std::size_t>(cellsX) * cellsY, rate);
}

struct CallLog
{
  std::vector<CoarseFragment> fragments;
  unsigned calls = 0;
  bool discard = false;
  u32 colour = ImageData::makePixel(10, 20, 30, 255);
};

CoarseShader loggingShader(CallLog& log)
{
  return [&](const CoarseFragment& fragment, u32& out) -> bool {
    ++log.calls;
    log.fragments.push_back(fragment);
    if (log.discard) {
      return false;
    }
    out = log.colour;
    return true;
  };
}

Triangle makeTriangle(const double win[3][2], const double depths[3],
                      const Vector2f uvs[3],
                      const Vector3f& rgb = Vector3f(1.0f, 1.0f, 1.0f),
                      const Vector3f& normal = Vector3f(0.0f, 0.0f, 1.0f),
                      unsigned width = CW, unsigned height = CH)
{
  return Triangle(
    winVertexAt(win[0][0], win[0][1], depths[0], width, height,
                uvs[0], rgb, normal),
    winVertexAt(win[1][0], win[1][1], depths[1], width, height,
                uvs[1], rgb, normal),
    winVertexAt(win[2][0], win[2][1], depths[2], width, height,
                uvs[2], rgb, normal));
}

// Large triangle that covers every pixel of the 16x16 target on its own.
const double kCoverWin[3][2] = {
  {0.0, 16.0}, {32.0, 16.0}, {0.0, -16.0}
};

} // namespace

// ---------------------------------------------------------------------------
// grouping
// ---------------------------------------------------------------------------

static int group_rate4_blocks_share_one_shader_call()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer(target, depth);

  const double win[3][2] = {{0, 16}, {32, 16}, {0, -16}};
  const double depths[3] = {1.0, 2.5, 4.0}; // deliberately unequal w
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};
  CallLog log;
  renderer.submit(makeTriangle(win, depths, uvs), loggingShader(log));
  renderer.resolve(uniformRates(CW, CH, 4));

  const auto expected = coveredPixels(win, CW, CH);
  TASK_CHECK(renderer.rounds == 1);
  TASK_CHECK(renderer.captured_fragments == expected.size());
  TASK_CHECK(renderer.committed_pixels == expected.size());
  TASK_CHECK(renderer.rasterized_primitives == 1); // geometry walked once

  // One shader call per distinct 4x4 block touched by the triangle.
  std::vector<std::pair<int, int>> blocks;
  std::uint32_t maskPixels = 0;
  for (const CoarseFragment& fragment : log.fragments) {
    TASK_CHECK(fragment.rate == 4);
    TASK_CHECK(fragment.blockX % 4 == 0 && fragment.blockY % 4 == 0);
    TASK_CHECK(fragment.coverageMask != 0);
    blocks.emplace_back(fragment.blockX, fragment.blockY);
    maskPixels += static_cast<std::uint32_t>(popCount(fragment.coverageMask));
  }
  std::sort(blocks.begin(), blocks.end());
  TASK_CHECK(std::adjacent_find(blocks.begin(), blocks.end()) == blocks.end());
  TASK_CHECK(maskPixels == expected.size());
  TASK_CHECK(log.calls == blocks.size());
  TASK_CHECK(renderer.shader_calls == log.calls);
  // Real coarse shading: every block shades several pixels with one call,
  // so the callback count is strictly below the per-pixel coverage count.
  TASK_CHECK(log.calls < expected.size());
  return 0;
}

static int group_clipped_fragments_share_blocks()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());

  // One vertex behind the near plane and one past the side plane, so the
  // six-plane clipper has to produce several fragments of the same
  // primitive. Vertex order is the front-facing winding for y-down screens.
  Triangle clip;
  clip.vertices[0] = winVertex(2.0, 2.0, 1.0);
  clip.vertices[0].pos.z() = -2.0f; // z/w < -1: behind the near plane
  clip.vertices[1] = winVertex(3.0, 14.0, 3.0);
  clip.vertices[2] = winVertex(30.0, 3.0, 2.0); // ndc x > 1: past the side
  for (int i = 0; i < 3; ++i) {
    clip.vertices[i].uv = Vector2f(static_cast<float>(i), 0.0f);
  }

  const std::vector<Triangle> pieces =
    ClipSpace::clipByAllPlanes(std::vector<Triangle>{clip});
  TASK_CHECK(pieces.size() >= 2); // clipping really fragmented the primitive

  CoarseRenderer renderer(target, depth);
  CallLog log;
  renderer.submit(clip, loggingShader(log));
  renderer.resolve(uniformRates(CW, CH, 4));

  TASK_CHECK(renderer.rasterized_primitives == 1);
  TASK_CHECK(renderer.captured_fragments == renderer.committed_pixels);

  // Internal clip edges must not split a primitive/block group.
  std::vector<std::pair<int, int>> blocks;
  for (const CoarseFragment& fragment : log.fragments) {
    TASK_CHECK(fragment.primitive == 0);
    blocks.emplace_back(fragment.blockX, fragment.blockY);
  }
  std::sort(blocks.begin(), blocks.end());
  TASK_CHECK(std::adjacent_find(blocks.begin(), blocks.end()) == blocks.end());
  TASK_CHECK(log.calls == blocks.size());
  TASK_CHECK(log.calls >= 1);
  return 0;
}

static int group_mixed_rates_on_partial_canvas()
{
  const unsigned width = 19u;  // not a multiple of 8 or any rate
  const unsigned height = 13u;
  ImageData target(width, height);
  ZBuffer depth(width, height);
  depth.fill(std::numeric_limits<double>::infinity());

  const unsigned cellsX = (width + 7u) / 8u;  // 3
  const unsigned cellsY = (height + 7u) / 8u; // 2
  ShadingRateMap rates(static_cast<std::size_t>(cellsX) * cellsY);
  const int pattern[2][3] = {{1, 4, 2}, {2, 1, 4}};
  for (unsigned cy = 0; cy < cellsY; ++cy) {
    for (unsigned cx = 0; cx < cellsX; ++cx) {
      rates[cy * cellsX + cx] = pattern[cy][cx];
    }
  }

  const double win[3][2] = {
    {0.0, static_cast<double>(height)},
    {2.0 * width, static_cast<double>(height)},
    {0.0, -static_cast<double>(height)}
  };
  const double depths[3] = {1.5, 2.5, 3.5};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};

  CoarseRenderer renderer(target, depth);
  CallLog log;
  renderer.submit(makeTriangle(win, depths, uvs,
                               Vector3f(1.0f, 1.0f, 1.0f),
                               Vector3f(0.0f, 0.0f, 1.0f),
                               width, height),
                  loggingShader(log));
  renderer.resolve(rates);

  const auto expected = coveredPixels(win, width, height);
  TASK_CHECK(expected.size() > 0);
  std::uint32_t seen[3] = {0, 0, 0};
  std::uint32_t covered = 0;
  for (const CoarseFragment& fragment : log.fragments) {
    TASK_CHECK(fragment.rate == 1 || fragment.rate == 2 || fragment.rate == 4);
    TASK_CHECK(fragment.blockX % fragment.rate == 0);
    TASK_CHECK(fragment.blockY % fragment.rate == 0);
    const int rateIndex = fragment.rate == 1 ? 0 :
                          fragment.rate == 2 ? 1 : 2;
    seen[rateIndex] = 1;
    covered += static_cast<std::uint32_t>(popCount(fragment.coverageMask));

    // Every set bit must address a pixel that actually exists and lie in
    // the rate's screen-origin-aligned block.
    std::uint32_t mask = fragment.coverageMask;
    while (mask) {
      const int bit = lowestBitIndex(mask);
      mask &= mask - 1;
      const int i = bit % fragment.rate;
      const int j = bit / fragment.rate;
      const int px = fragment.blockX + i;
      const int py = fragment.blockY + j;
      TASK_CHECK(px >= 0 && px < static_cast<int>(width));
      TASK_CHECK(py >= 0 && py < static_cast<int>(height));
      TASK_CHECK(px / fragment.rate == fragment.blockX / fragment.rate);
      TASK_CHECK(py / fragment.rate == fragment.blockY / fragment.rate);
    }
  }
  TASK_CHECK(seen[0] && seen[1] && seen[2]); // all three rates exercised
  TASK_CHECK(covered == expected.size());
  return 0;
}

static int group_rate1_runs_same_frontier_flow()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer(target, depth);

  CallLog log;
  const double depths[3] = {2.0, 2.0, 2.0};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};
  renderer.submit(makeTriangle(kCoverWin, depths, uvs), loggingShader(log));
  renderer.resolve(uniformRates(CW, CH, 1));

  TASK_CHECK(renderer.rounds == 1);
  // Rate 1 still goes through grouping: exactly one call per covered pixel,
  // each block is a single pixel with a single-bit mask.
  TASK_CHECK(log.calls == static_cast<unsigned>(CW * CH));
  TASK_CHECK(renderer.committed_pixels == CW * CH);
  for (const CoarseFragment& fragment : log.fragments) {
    TASK_CHECK(fragment.rate == 1);
    TASK_CHECK(fragment.coverageMask == 1u);
    TASK_CHECK(fragment.blockX == static_cast<int>(fragment.sampleX));
    TASK_CHECK(fragment.blockY == static_cast<int>(fragment.sampleY));
  }
  return 0;
}

static int group_invalid_triangle_skipped_without_index()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer(target, depth);

  const double depths[3] = {1.0, 1.0, 1.0};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};
  Triangle badW = makeTriangle(kCoverWin, depths, uvs);
  badW.vertices[1].pos.w() = 0.0f;
  Triangle badNan = makeTriangle(kCoverWin, depths, uvs);
  badNan.vertices[0].pos.x() = std::numeric_limits<float>::quiet_NaN();
  Triangle badInf = makeTriangle(kCoverWin, depths, uvs);
  badInf.vertices[2].pos.y() = std::numeric_limits<float>::infinity();

  CallLog rejected; // rejected submits never invoke their shader
  renderer.submit(badW, loggingShader(rejected));
  renderer.submit(badNan, loggingShader(rejected));
  renderer.submit(badInf, loggingShader(rejected));

  CallLog valid;
  renderer.submit(makeTriangle(kCoverWin, depths, uvs), loggingShader(valid));
  renderer.resolve(uniformRates(CW, CH, 2));

  TASK_CHECK(valid.calls > 0);
  TASK_CHECK(renderer.rasterized_primitives == 1);
  for (const CoarseFragment& fragment : valid.fragments) {
    TASK_CHECK(fragment.primitive == 0); // rejects consumed no index
  }
  return 0;
}

// ---------------------------------------------------------------------------
// interpolation
// ---------------------------------------------------------------------------

static int inter_perspective_depth_uv_rgb_normal()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());

  const double win[3][2] = {{0, 16}, {32, 16}, {0, -16}};
  const double w[3] = {1.0, 2.0, 4.0};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};
  const Vector3f rgb(0.25f, 0.5f, 0.75f);
  const Vector3f normal(0.0f, 0.0f, 2.0f); // deliberately not unit length
  Triangle triangle = makeTriangle(win, w, uvs, rgb, normal);

  CoarseRenderer renderer(target, depth);
  CallLog log;
  renderer.submit(triangle, loggingShader(log));
  renderer.resolve(uniformRates(CW, CH, 1));

  const int px = 3;
  const int py = 4;
  const CoarseFragment* found = nullptr;
  for (const CoarseFragment& fragment : log.fragments) {
    if (fragment.blockX == px && fragment.blockY == py) {
      found = &fragment;
    }
  }
  TASK_CHECK(found != nullptr);

  double lambda[3];
  windowBary(win, px + 0.5, py + 0.5, lambda);
  double invW = 0.0, expU = 0.0, expV = 0.0;
  for (int i = 0; i < 3; ++i) {
    invW += lambda[i] / w[i];
    expU += lambda[i] * uvs[i].x() / w[i];
    expV += lambda[i] * uvs[i].y() / w[i];
  }
  const double expectedW = 1.0 / invW;
  expU *= expectedW;
  expV *= expectedW;

  TASK_CHECK(std::fabs(found->depth - expectedW) < 1e-9);
  TASK_CHECK(std::fabs(found->sampleX - (px + 0.5)) < 1e-12);
  TASK_CHECK(std::fabs(found->sampleY - (py + 0.5)) < 1e-12);
  TASK_CHECK(std::fabs(found->uv.x() - expU) < 1e-6);
  TASK_CHECK(std::fabs(found->uv.y() - expV) < 1e-6);
  TASK_CHECK(std::fabs(found->rgb.x() - 0.25f) < 1e-6);
  TASK_CHECK(std::fabs(found->rgb.y() - 0.5f) < 1e-6);
  TASK_CHECK(std::fabs(found->rgb.z() - 0.75f) < 1e-6);
  // Normal must be passed through unnormalized.
  TASK_CHECK(std::fabs(found->normal.x()) < 1e-6);
  TASK_CHECK(std::fabs(found->normal.y()) < 1e-6);
  TASK_CHECK(std::fabs(found->normal.z() - 2.0f) < 1e-6);
  return 0;
}

static int inter_analytic_uv_derivatives()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());

  const double win[3][2] = {{0, 16}, {32, 16}, {0, -16}};
  const double w[3] = {1.0, 3.0, 2.0};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(2, 0), Vector2f(0, 3)};
  CoarseRenderer renderer(target, depth);
  CallLog log;
  renderer.submit(makeTriangle(win, w, uvs), loggingShader(log));
  renderer.resolve(uniformRates(CW, CH, 1));

  auto uvAt = [&](double sx, double sy, double& u, double& v) {
    double lambda[3];
    windowBary(win, sx, sy, lambda);
    double invW = 0.0, hu = 0.0, hv = 0.0;
    for (int i = 0; i < 3; ++i) {
      invW += lambda[i] / w[i];
      hu += lambda[i] * uvs[i].x() / w[i];
      hv += lambda[i] * uvs[i].y() / w[i];
    }
    const double depth = 1.0 / invW;
    u = hu * depth;
    v = hv * depth;
  };

  const int px = 5;
  const int py = 2;
  const CoarseFragment* found = nullptr;
  for (const CoarseFragment& fragment : log.fragments) {
    if (fragment.blockX == px && fragment.blockY == py) {
      found = &fragment;
    }
  }
  TASK_CHECK(found != nullptr);

  double u0, v0, ux, vx, uy, vy;
  uvAt(px + 0.5, py + 0.5, u0, v0);
  uvAt(px + 1.5, py + 0.5, ux, vx);
  uvAt(px + 0.5, py + 1.5, uy, vy);

  TASK_CHECK(std::fabs(found->duvdx.x() - (ux - u0)) < 1e-6);
  TASK_CHECK(std::fabs(found->duvdx.y() - (vx - v0)) < 1e-6);
  TASK_CHECK(std::fabs(found->duvdy.x() - (uy - u0)) < 1e-6);
  TASK_CHECK(std::fabs(found->duvdy.y() - (vy - v0)) < 1e-6);
  return 0;
}

static int inter_derivatives_not_scaled_by_rate()
{
  const double win[3][2] = {{0, 16}, {32, 16}, {0, -16}};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};

  // With constant clip w the perspective interpolation is affine in window
  // space, so the single-pixel UV derivative is the same at every position
  // and independent of the shading rate.
  const double uniformW[3] = {3.0, 3.0, 3.0};

  ImageData target1(CW, CH);
  ZBuffer depth1(CW, CH);
  depth1.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer1(target1, depth1);
  CallLog log1;
  renderer1.submit(makeTriangle(win, uniformW, uvs), loggingShader(log1));
  renderer1.resolve(uniformRates(CW, CH, 1));

  ImageData target4(CW, CH);
  ZBuffer depth4(CW, CH);
  depth4.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer4(target4, depth4);
  CallLog log4;
  renderer4.submit(makeTriangle(win, uniformW, uvs), loggingShader(log4));
  renderer4.resolve(uniformRates(CW, CH, 4));

  // Analytic single-pixel derivatives of the affine UV field.
  auto affineUv = [&](double sx, double sy, double& u, double& v) {
    double lambda[3];
    windowBary(win, sx, sy, lambda);
    u = lambda[0] * uvs[0].x() + lambda[1] * uvs[1].x() +
        lambda[2] * uvs[2].x();
    v = lambda[0] * uvs[0].y() + lambda[1] * uvs[1].y() +
        lambda[2] * uvs[2].y();
  };
  double u0, v0, ux, vx, uy, vy;
  affineUv(2.5, 3.5, u0, v0);
  affineUv(3.5, 3.5, ux, vx);
  affineUv(2.5, 4.5, uy, vy);
  const double analyticDudx = ux - u0;
  const double analyticDvdy = vy - v0;

  TASK_CHECK(!log1.fragments.empty() && !log4.fragments.empty());
  const CoarseFragment& firstRate1 = log1.fragments.front();
  for (const CoarseFragment& fragment : log1.fragments) {
    TASK_CHECK(std::fabs(fragment.duvdx.x() - analyticDudx) < 1e-6);
    TASK_CHECK(std::fabs(fragment.duvdy.y() - analyticDvdy) < 1e-6);
  }
  for (const CoarseFragment& fragment : log4.fragments) {
    // A rate-scaled implementation would report roughly 4x these values.
    TASK_CHECK(std::fabs(fragment.duvdx.x() - analyticDudx) < 1e-6);
    TASK_CHECK(std::fabs(fragment.duvdy.y() - analyticDvdy) < 1e-6);
    // Affine field: rate 1 and rate 4 agree exactly on the same derivative.
    TASK_CHECK(std::fabs(fragment.duvdx.x() - firstRate1.duvdx.x()) < 1e-6);
    TASK_CHECK(std::fabs(fragment.duvdy.y() - firstRate1.duvdy.y()) < 1e-6);
  }

  // Non-uniform w: the rate-4 fragment must still use a one-pixel step,
  // evaluated at its own representative position.
  const double perspectiveW[3] = {2.0, 5.0, 3.0};
  ImageData targetP(CW, CH);
  ZBuffer depthP(CW, CH);
  depthP.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer rendererP(targetP, depthP);
  CallLog logP;
  rendererP.submit(makeTriangle(win, perspectiveW, uvs), loggingShader(logP));
  rendererP.resolve(uniformRates(CW, CH, 4));

  auto perspectiveUv = [&](double sx, double sy, double& u, double& v) {
    double lambda[3];
    windowBary(win, sx, sy, lambda);
    double invW = 0.0, hu = 0.0, hv = 0.0;
    for (int i = 0; i < 3; ++i) {
      invW += lambda[i] / perspectiveW[i];
      hu += lambda[i] * uvs[i].x() / perspectiveW[i];
      hv += lambda[i] * uvs[i].y() / perspectiveW[i];
    }
    const double depth = 1.0 / invW;
    u = hu * depth;
    v = hv * depth;
  };

  for (const CoarseFragment& fragment : logP.fragments) {
    double hereU, hereV, stepU, stepV, farU, farV;
    perspectiveUv(fragment.sampleX, fragment.sampleY, hereU, hereV);
    perspectiveUv(fragment.sampleX + 1.0, fragment.sampleY, stepU, stepV);
    // A four-pixel (rate-scaled) step gives a different, larger difference.
    perspectiveUv(fragment.sampleX + 4.0, fragment.sampleY, farU, farV);
    TASK_CHECK(std::fabs(fragment.duvdx.x() - (stepU - hereU)) < 1e-6);
    TASK_CHECK(std::fabs(fragment.duvdx.x() - (farU - hereU)) > 1e-4);
  }
  return 0;
}

static int inter_representative_position_is_pixel_mean()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());

  const double win[3][2] = {{0, 16}, {32, 16}, {0, -16}};
  const double depths[3] = {1.0, 2.0, 3.0};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};
  CoarseRenderer renderer(target, depth);
  CallLog log;
  renderer.submit(makeTriangle(win, depths, uvs), loggingShader(log));
  renderer.resolve(uniformRates(CW, CH, 4));

  for (const CoarseFragment& fragment : log.fragments) {
    double sumX = 0.0;
    double sumY = 0.0;
    int count = 0;
    std::uint32_t mask = fragment.coverageMask;
    while (mask) {
      const int bit = lowestBitIndex(mask);
      mask &= mask - 1;
      const int i = bit % fragment.rate;
      const int j = bit / fragment.rate;
      sumX += fragment.blockX + i + 0.5;
      sumY += fragment.blockY + j + 0.5;
      ++count;
    }
    TASK_CHECK(count > 0);
    TASK_CHECK(std::fabs(fragment.sampleX - sumX / count) < 1e-12);
    TASK_CHECK(std::fabs(fragment.sampleY - sumY / count) < 1e-12);
    TASK_CHECK(fragment.round == 0);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// visibility
// ---------------------------------------------------------------------------

static int vis_equal_depth_prefers_earlier_submission()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer(target, depth);

  const double depths[3] = {3.0, 3.0, 3.0}; // equal reconstructed w
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};
  CallLog first;
  first.colour = ImageData::makePixel(200, 10, 10, 255);
  CallLog second;
  second.colour = ImageData::makePixel(10, 10, 200, 255);
  renderer.submit(makeTriangle(kCoverWin, depths, uvs), loggingShader(first));
  renderer.submit(makeTriangle(kCoverWin, depths, uvs), loggingShader(second));
  renderer.resolve(uniformRates(CW, CH, 4));

  // Both primitives still produce a candidate per pixel, but the later one
  // is fully hidden and must never run a shader.
  TASK_CHECK(renderer.captured_fragments == 2u * CW * CH);
  TASK_CHECK(first.calls > 0);
  TASK_CHECK(second.calls == 0);
  TASK_CHECK(renderer.committed_pixels == CW * CH);
  for (unsigned y = 0; y < CH; ++y) {
    for (unsigned x = 0; x < CW; ++x) {
      TASK_CHECK(target.getPixel(x, y) == first.colour);
    }
  }
  return 0;
}

static int vis_baseline_occlusion_is_strict()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());
  const u32 baseline = ImageData::makePixel(7, 8, 9, 255);

  // Pre-commit a wall at w = 2 over the left 8 columns.
  for (unsigned y = 0; y < CH; ++y) {
    for (unsigned x = 0; x < 8u; ++x) {
      depth.unsafeSet(x, y, 2.0);
      target.setPixel(x, y, baseline);
    }
  }

  CoarseRenderer renderer(target, depth);
  CallLog log;
  log.colour = ImageData::makePixel(90, 90, 90, 255);
  const double depths[3] = {2.0, 2.0, 2.0}; // equal, not strictly nearer
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};
  renderer.submit(makeTriangle(kCoverWin, depths, uvs), loggingShader(log));
  renderer.resolve(uniformRates(CW, CH, 2));

  for (const CoarseFragment& fragment : log.fragments) {
    // No group may contain a baseline-blocked pixel.
    std::uint32_t mask = fragment.coverageMask;
    while (mask) {
      const int bit = lowestBitIndex(mask);
      mask &= mask - 1;
      const int px = fragment.blockX + bit % fragment.rate;
      TASK_CHECK(px >= 8);
    }
  }
  for (unsigned y = 0; y < CH; ++y) {
    for (unsigned x = 0; x < 8u; ++x) {
      TASK_CHECK(target.getPixel(x, y) == baseline);
      TASK_CHECK(depth.unsafeGet(x, y) == 2.0);
    }
  }
  TASK_CHECK(renderer.committed_pixels == 8u * CH);
  return 0;
}

static int vis_three_layers_advance_frontier_masks()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer(target, depth);

  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};
  const double backW[3] = {6.0, 6.0, 6.0};
  const double middleW[3] = {4.0, 4.0, 4.0};
  const double frontW[3] = {2.0, 2.0, 2.0};
  const double hiddenW[3] = {8.0, 8.0, 8.0};

  // Front is a fully in-viewport triangle covering a shrinking left strip,
  // so block (4, 0) is split between front-covered columns and columns only
  // the middle layer reaches. Its winding matches the y-down rasterizer.
  const double frontWin[3][2] = {{0, 0}, {0, 16}, {6, 0}};

  CallLog backLog;
  backLog.colour = ImageData::makePixel(10, 10, 220, 255);
  CallLog middleLog;
  middleLog.discard = true;
  CallLog frontLog;
  frontLog.discard = true;
  CallLog hiddenLog; // fully behind the back layer
  hiddenLog.colour = ImageData::makePixel(1, 2, 3, 255);

  renderer.submit(makeTriangle(frontWin, frontW, uvs), loggingShader(frontLog));
  renderer.submit(makeTriangle(kCoverWin, middleW, uvs),
                  loggingShader(middleLog));
  renderer.submit(makeTriangle(kCoverWin, backW, uvs), loggingShader(backLog));
  renderer.submit(makeTriangle(kCoverWin, hiddenW, uvs),
                  loggingShader(hiddenLog));
  renderer.resolve(uniformRates(CW, CH, 4));

  TASK_CHECK(renderer.rounds == 3);
  TASK_CHECK(hiddenLog.calls == 0);                 // fully occluded
  TASK_CHECK(frontLog.calls > 0);
  TASK_CHECK(middleLog.calls > 0);
  TASK_CHECK(backLog.calls > 0);
  TASK_CHECK(renderer.committed_pixels == CW * CH);

  // The middle primitive must meet block (4, 0) twice with different masks
  // and different representative positions. Round 0 covers the columns the
  // front misses; round 1 covers the columns the front discarded.
  std::uint32_t frontMask = 0;
  std::uint32_t blockMask = 0;
  for (int j = 0; j < 4; ++j) {
    for (int i = 0; i < 4; ++i) {
      const int px = 4 + i;
      const int py = j;
      blockMask |= static_cast<std::uint32_t>(1u) << (j * 4 + i);
      if (insideTriangle(frontWin, px + 0.5, py + 0.5)) {
        frontMask |= static_cast<std::uint32_t>(1u) << (j * 4 + i);
      }
    }
  }
  TASK_CHECK(frontMask != 0 && frontMask != blockMask); // block really splits

  std::vector<const CoarseFragment*> middleBlock;
  for (const CoarseFragment& fragment : middleLog.fragments) {
    if (fragment.blockX == 4 && fragment.blockY == 0) {
      middleBlock.push_back(&fragment);
    }
  }
  TASK_CHECK(middleBlock.size() == 2);
  TASK_CHECK(middleBlock[0]->round == 0 && middleBlock[1]->round == 1);
  TASK_CHECK(middleBlock[0]->coverageMask != middleBlock[1]->coverageMask);
  TASK_CHECK(middleBlock[0]->sampleX != middleBlock[1]->sampleX);
  TASK_CHECK(middleBlock[0]->coverageMask == (blockMask & ~frontMask));
  TASK_CHECK(middleBlock[1]->coverageMask == frontMask);

  // Every pixel is committed exactly once and ends up with the back colour.
  std::vector<int> commitCount(CW * CH, 0);
  std::uint32_t backPixels = 0;
  for (const CoarseFragment& fragment : backLog.fragments) {
    backPixels += static_cast<std::uint32_t>(popCount(fragment.coverageMask));
    std::uint32_t mask = fragment.coverageMask;
    while (mask) {
      const int bit = lowestBitIndex(mask);
      mask &= mask - 1;
      const int px = fragment.blockX + bit % fragment.rate;
      const int py = fragment.blockY + bit / fragment.rate;
      ++commitCount[py * CW + px];
    }
  }
  TASK_CHECK(backPixels == CW * CH);
  for (int count : commitCount) {
    TASK_CHECK(count == 1); // accepted pixels are never re-shaded
  }
  for (unsigned y = 0; y < CH; ++y) {
    for (unsigned x = 0; x < CW; ++x) {
      TASK_CHECK(target.getPixel(x, y) == backLog.colour);
    }
  }
  return 0;
}

static int vis_bad_rate_map_and_resized_target_can_retry()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer(target, depth);

  const double depths[3] = {1.5, 1.5, 1.5};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};
  CallLog log;
  renderer.submit(makeTriangle(kCoverWin, depths, uvs), loggingShader(log));

  bool threw = false;
  try {
    renderer.resolve(ShadingRateMap{1, 2, 4}); // wrong element count
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(log.calls == 0); // no callback before the rejection

  ShadingRateMap badValue = uniformRates(CW, CH, 1);
  badValue[0] = 3;
  threw = false;
  try {
    renderer.resolve(badValue);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(log.calls == 0);

  // Resized target is rejected, then restoring the fixed size allows the
  // retained batch to run.
  target.create(CW + 1u, CH);
  threw = false;
  try {
    renderer.resolve(uniformRates(CW, CH, 2));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  target.create(CW, CH);
  renderer.resolve(uniformRates(CW, CH, 2));
  TASK_CHECK(log.calls > 0);
  TASK_CHECK(renderer.committed_pixels == CW * CH);
  return 0;
}

static int vis_reset_and_consecutive_batches_stay_independent()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer(target, depth);

  const double depths[3] = {2.0, 2.0, 2.0};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};

  CallLog dropped;
  renderer.submit(makeTriangle(kCoverWin, depths, uvs), loggingShader(dropped));
  renderer.reset();
  renderer.resolve(uniformRates(CW, CH, 4));
  TASK_CHECK(renderer.rounds == 0);
  TASK_CHECK(renderer.captured_fragments == 0);
  TASK_CHECK(renderer.shader_calls == 0);
  TASK_CHECK(dropped.calls == 0);
  for (unsigned y = 0; y < CH; ++y) {
    for (unsigned x = 0; x < CW; ++x) {
      TASK_CHECK(target.getPixel(x, y) == 0u);
    }
  }

  // First real batch becomes the baseline; indices restart at 0.
  CallLog first;
  first.colour = ImageData::makePixel(12, 34, 56, 255);
  renderer.submit(makeTriangle(kCoverWin, depths, uvs), loggingShader(first));
  renderer.resolve(uniformRates(CW, CH, 4));
  TASK_CHECK(first.fragments.front().primitive == 0);
  TASK_CHECK(renderer.committed_pixels == CW * CH);

  // A second batch behind the committed depth is entirely occluded and
  // again starts its submission indices at 0 (proven on a visible batch).
  CallLog behind;
  const double farDepths[3] = {5.0, 5.0, 5.0};
  renderer.submit(makeTriangle(kCoverWin, farDepths, uvs),
                  loggingShader(behind));
  renderer.resolve(uniformRates(CW, CH, 4));
  TASK_CHECK(behind.calls == 0);
  TASK_CHECK(renderer.captured_fragments == 0);
  TASK_CHECK(renderer.committed_pixels == 0);
  for (unsigned y = 0; y < CH; ++y) {
    for (unsigned x = 0; x < CW; ++x) {
      TASK_CHECK(target.getPixel(x, y) == first.colour);
    }
  }

  // Pixels without baseline content still accept the next batch, and its
  // primitive index restarts at 0.
  ImageData target2(8u, 8u);
  ZBuffer depth2(8u, 8u);
  depth2.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer2(target2, depth2);
  CallLog nextBatch;
  const double smallWin[3][2] = {{0, 8}, {16, 8}, {0, -8}};
  renderer2.submit(makeTriangle(smallWin, depths, uvs),
                   loggingShader(nextBatch));
  renderer2.resolve(uniformRates(8u, 8u, 2));
  TASK_CHECK(nextBatch.calls > 0);
  TASK_CHECK(nextBatch.fragments.front().primitive == 0);
  return 0;
}

static int vis_alpha_zero_is_treated_as_discard()
{
  ImageData target(CW, CH);
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer(target, depth);

  const double frontW[3] = {2.0, 2.0, 2.0};
  const double backW[3] = {5.0, 5.0, 5.0};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};

  // The front shader accepts but reports alpha 0; by contract that is the
  // same as returning false and must let the back layer show through.
  CallLog frontLog;
  frontLog.colour = ImageData::makePixel(200, 200, 200, 0);
  CallLog backLog;
  backLog.colour = ImageData::makePixel(30, 60, 90, 255);

  renderer.submit(makeTriangle(kCoverWin, frontW, uvs),
                  loggingShader(frontLog));
  renderer.submit(makeTriangle(kCoverWin, backW, uvs), loggingShader(backLog));
  renderer.resolve(uniformRates(CW, CH, 4));

  TASK_CHECK(renderer.rounds == 2);
  TASK_CHECK(frontLog.calls > 0 && backLog.calls > 0);
  TASK_CHECK(frontLog.fragments.front().round == 0);
  TASK_CHECK(backLog.fragments.front().round == 1);
  TASK_CHECK(renderer.committed_pixels == CW * CH);
  for (unsigned y = 0; y < CH; ++y) {
    for (unsigned x = 0; x < CW; ++x) {
      TASK_CHECK(target.getPixel(x, y) == backLog.colour);
    }
  }
  return 0;
}

static int vis_mismatched_depth_dimensions_rejected()
{
  ImageData target(CW, CH);
  ZBuffer wrongDepth(CW, CH - 1u); // deliberately mismatched
  wrongDepth.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer(target, wrongDepth);

  const double depths[3] = {2.0, 2.0, 2.0};
  const Vector2f uvs[3] = {Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)};
  CallLog log;
  renderer.submit(makeTriangle(kCoverWin, depths, uvs), loggingShader(log));

  bool threw = false;
  try {
    renderer.resolve(uniformRates(CW, CH, 4));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(log.calls == 0); // rejected before any callback or write
  for (unsigned y = 0; y < CH; ++y) {
    for (unsigned x = 0; x < CW; ++x) {
      TASK_CHECK(target.getPixel(x, y) == 0u);
    }
  }

  // Swapping in a correctly sized depth buffer (new renderer, same target)
  // lets an equivalent batch run.
  ZBuffer depth(CW, CH);
  depth.fill(std::numeric_limits<double>::infinity());
  CoarseRenderer renderer2(target, depth);
  CallLog okLog;
  renderer2.submit(makeTriangle(kCoverWin, depths, uvs), loggingShader(okLog));
  renderer2.resolve(uniformRates(CW, CH, 4));
  TASK_CHECK(okLog.calls > 0);
  TASK_CHECK(renderer2.committed_pixels == CW * CH);
  return 0;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

extern const struct task_case task_cases[] = {
  {"grouping", "rate4_blocks_share_one_shader_call",
   group_rate4_blocks_share_one_shader_call},
  {"grouping", "clipped_fragments_share_blocks",
   group_clipped_fragments_share_blocks},
  {"grouping", "mixed_rates_on_partial_canvas",
   group_mixed_rates_on_partial_canvas},
  {"grouping", "rate1_same_frontier_flow",
   group_rate1_runs_same_frontier_flow},
  {"grouping", "invalid_triangle_skipped_without_index",
   group_invalid_triangle_skipped_without_index},

  {"interpolation", "perspective_depth_uv_rgb_normal",
   inter_perspective_depth_uv_rgb_normal},
  {"interpolation", "analytic_uv_derivatives",
   inter_analytic_uv_derivatives},
  {"interpolation", "derivatives_not_scaled_by_rate",
   inter_derivatives_not_scaled_by_rate},
  {"interpolation", "representative_position_is_pixel_mean",
   inter_representative_position_is_pixel_mean},

  {"visibility", "equal_depth_prefers_earlier_submission",
   vis_equal_depth_prefers_earlier_submission},
  {"visibility", "baseline_occlusion_is_strict",
   vis_baseline_occlusion_is_strict},
  {"visibility", "three_layers_advance_frontier_masks",
   vis_three_layers_advance_frontier_masks},
  {"visibility", "bad_rate_map_and_resized_target_can_retry",
   vis_bad_rate_map_and_resized_target_can_retry},
  {"visibility", "reset_and_consecutive_batches_stay_independent",
   vis_reset_and_consecutive_batches_stay_independent},
  {"visibility", "alpha_zero_is_treated_as_discard",
   vis_alpha_zero_is_treated_as_discard},
  {"visibility", "mismatched_depth_dimensions_rejected",
   vis_mismatched_depth_dimensions_rejected},
};

extern const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
