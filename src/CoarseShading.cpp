#include "CoarseShading.h"
#include "ClipSpace.h"
#include "Graphics3D.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace al { namespace graphics {

CoarseRenderer::CoarseRenderer(ImageData& target, ZBuffer& depth)
: captured_fragments(0)
, rasterized_primitives(0)
, frontier_pixels(0)
, shader_calls(0)
, committed_pixels(0)
, rounds(0)
, target_(target)
, depth_(depth)
, targetWidth_(target.width())
, targetHeight_(target.height())
, nextSubmission_(0)
{
}
bool CoarseRenderer::finiteTriangle(const Triangle& triangle)
{
  for (int i = 0; i < 3; ++i) {
    const Vector4f& p = triangle.vertices[i].pos;
    if (!std::isfinite(p.x()) || !std::isfinite(p.y()) ||
        !std::isfinite(p.z()) || !std::isfinite(p.w())) {
      return false;
    }
    if (!(p.w() > 0.0f)) {
      return false;
    }
  }
  return true;
}

void CoarseRenderer::submit(const Triangle& clipTriangle,
                            const CoarseShader& shader)
{
  if (!finiteTriangle(clipTriangle)) {
    return;
  }
  PendingPrimitive pending;
  pending.triangle = clipTriangle;
  pending.shader = shader;
  pending.index = nextSubmission_++;
  pending_.push_back(std::move(pending));
}

void CoarseRenderer::reset()
{
  pending_.clear();
  nextSubmission_ = 0;
}

// Perspective-correct attribute evaluation at a window-space point, using
// the original (unclipped) clip-space triangle so internal clip edges never
// split a shading group. All math is done in double; only the returned
// fragment attributes are stored in float.
void CoarseRenderer::interpolateAttributes(
  const PendingPrimitive& primitive,
  int width, int height,
  double sampleX, double sampleY,
  CoarseFragment& fragment)
{
  const Triangle& clip = primitive.triangle;

  double wx[3], wy[3], invClipW[3];
  for (int i = 0; i < 3; ++i) {
    const double cw = clip.vertices[i].pos.w();
    const double ndcX = clip.vertices[i].pos.x() / cw;
    const double ndcY = clip.vertices[i].pos.y() / cw;
    wx[i] = width * 0.5 * ndcX + width * 0.5;
    wy[i] = (-ndcY) * height * 0.5 + height * 0.5;
    invClipW[i] = 1.0 / cw;
  }

  auto weightsAt = [&](double px, double py, double lambda[3]) -> bool {
    const double area =
      (wx[1] - wx[0]) * (wy[2] - wy[0]) -
      (wy[1] - wy[0]) * (wx[2] - wx[0]);
    if (area == 0.0) {
      lambda[0] = lambda[1] = lambda[2] = 0.0;
      return false;
    }
    auto crossAt = [&](int i, int j) {
      return (wx[j] - wx[i]) * (py - wy[i]) -
             (wy[j] - wy[i]) * (px - wx[i]);
    };
    // Same vertex ordering as G3D::crossProduct2D in the rasterizer.
    lambda[0] = crossAt(1, 2) / area;
    lambda[1] = crossAt(2, 0) / area;
    lambda[2] = crossAt(0, 1) / area;
    return true;
  };

  auto evaluate = [&](double px, double py,
                      double& outDepth, Vector2f& outUv,
                      Vector3f& outRgb, Vector3f& outNormal) {
    double lambda[3];
    weightsAt(px, py, lambda);
    double invW = 0.0;
    double u = 0.0, v = 0.0;
    double r = 0.0, g = 0.0, b = 0.0;
    double nx = 0.0, ny = 0.0, nz = 0.0;
    for (int i = 0; i < 3; ++i) {
      const Vertex4f& vert = clip.vertices[i];
      const double weighted = lambda[i] * invClipW[i];
      invW += weighted;
      u += weighted * vert.uv.x();
      v += weighted * vert.uv.y();
      r += weighted * vert.rgb.x();
      g += weighted * vert.rgb.y();
      b += weighted * vert.rgb.z();
      nx += weighted * vert.normal.x();
      ny += weighted * vert.normal.y();
      nz += weighted * vert.normal.z();
    }
    const double w = 1.0 / invW;
    outDepth = w;
    outUv = Vector2f(static_cast<float>(u * w),
                     static_cast<float>(v * w));
    outRgb = Vector3f(static_cast<float>(r * w),
                      static_cast<float>(g * w),
                      static_cast<float>(b * w));
    // The normal stays unnormalized by contract.
    outNormal = Vector3f(static_cast<float>(nx * w),
                         static_cast<float>(ny * w),
                         static_cast<float>(nz * w));
  };

  double depthHere;
  Vector2f uvHere, uvX, uvY;
  Vector3f rgbHere, normalHere, unused;
  evaluate(sampleX, sampleY, depthHere, uvHere, rgbHere, normalHere);
  double derivativeDepth; // discarded: derivatives are screen-space only
  evaluate(sampleX + 1.0, sampleY, derivativeDepth, uvX, unused, unused);
  evaluate(sampleX, sampleY + 1.0, derivativeDepth, uvY, unused, unused);

  fragment.sampleX = sampleX;
  fragment.sampleY = sampleY;
  fragment.depth = depthHere;
  fragment.uv = uvHere;
  fragment.duvdx = Vector2f(uvX.x() - uvHere.x(), uvX.y() - uvHere.y());
  fragment.duvdy = Vector2f(uvY.x() - uvHere.x(), uvY.y() - uvHere.y());
  fragment.rgb = rgbHere;
  fragment.normal = normalHere;
}

void CoarseRenderer::resolve(const ShadingRateMap& rates)
{
  const unsigned int width = targetWidth_;
  const unsigned int height = targetHeight_;
  const unsigned int cellsX = (width + 7u) / 8u;
  const unsigned int cellsY = (height + 7u) / 8u;

  // All validation happens before any callback can run or any pixel can be
  // written, so a rejected resolve leaves the pending batch retryable.
  if (target_.width() != width || target_.height() != height ||
      depth_.width() != width || depth_.height() != height) {
    throw std::invalid_argument(
      "CoarseRenderer::resolve target/depth dimensions changed");
  }
  if (rates.size() != static_cast<std::size_t>(cellsX) * cellsY) {
    throw std::invalid_argument(
      "CoarseRenderer::resolve shading rate map size mismatch");
  }
  for (int rate : rates) {
    if (rate != 1 && rate != 2 && rate != 4) {
      throw std::invalid_argument(
        "CoarseRenderer::resolve shading rate must be 1, 2 or 4");
    }
  }

  const std::size_t pixelCount = static_cast<std::size_t>(width) * height;

  // Freeze the baseline depth once; it is the only occlusion test used while
  // collecting candidates.
  std::vector<double> baselineDepth(pixelCount);
  for (unsigned int y = 0; y < height; ++y) {
    for (unsigned int x = 0; x < width; ++x) {
      baselineDepth[static_cast<std::size_t>(y) * width + x] =
        depth_.unsafeGet(x, y);
    }
  }

  // Geometry is walked exactly once per submitted primitive.
  std::vector<std::vector<Candidate>> candidates(pixelCount);
  std::size_t rasterized = 0;

  for (const PendingPrimitive& pending : pending_) {
    std::vector<Triangle> pieces =
      ClipSpace::clipByAllPlanes(std::vector<Triangle>{pending.triangle});
    if (pieces.empty()) {
      continue;
    }
    ++rasterized;

    for (Triangle piece : pieces) {
      piece.perspectiveDivide();
      for (int i = 0; i < 3; ++i) {
        G3D::ndcToWindow(piece[i].pos,
                         static_cast<float>(width),
                         static_cast<float>(height));
        // perspectiveDivide() leaves clip w in place; the BARYCENTRIC
        // production path inverts it before traversal so that
        // 1/(w0*alp + w1*bet + w2*gam) reconstructs clip w as depth.
        piece[i].pos.w() = 1.0f / piece[i].pos.w();
      }

      TriangleDrawer::forEachBarycentricCoverage(
        target_, piece,
        [&](const TriangleDrawer::Coverage& coverage) {
          const std::size_t pixel =
            static_cast<std::size_t>(coverage.y) * width + coverage.x;
          if (!(coverage.depth < baselineDepth[pixel])) {
            return;
          }
          // Clipped fragments of one original primitive produce at most one
          // candidate per pixel; keep the nearest fragment on overlap.
          for (Candidate& existing : candidates[pixel]) {
            if (existing.primitive == pending.index) {
              if (coverage.depth < existing.depth) {
                existing.depth = coverage.depth;
              }
              return;
            }
          }
          candidates[pixel].push_back(
            Candidate{pending.index, coverage.depth});
        });
    }
  }

  // Near to far, ties broken by submission order (earlier first).
  for (std::vector<Candidate>& list : candidates) {
    std::sort(list.begin(), list.end(),
              [](const Candidate& a, const Candidate& b) {
                if (a.depth != b.depth) {
                  return a.depth < b.depth;
                }
                return a.primitive < b.primitive;
              });
  }

  std::size_t uniqueCandidates = 0;
  for (const std::vector<Candidate>& list : candidates) {
    uniqueCandidates += list.size();
  }

  // Per-pixel frontier cursor plus the permanent accepted flag. Pixels with
  // no candidate simply keep the baseline and never enter a round.
  std::vector<std::size_t> cursor(pixelCount, 0);
  std::vector<unsigned char> accepted(pixelCount, 0);

  std::size_t totalFrontierPixels = 0;
  std::size_t totalShaderCalls = 0;
  std::size_t totalCommittedPixels = 0;
  unsigned int roundCount = 0;

  while (true) {
    // Key: (original primitive, block y, block x).
    std::map<std::tuple<std::size_t, int, int>, std::size_t> groupIndex;
    std::vector<Group> groups;

    for (unsigned int py = 0; py < height; ++py) {
      for (unsigned int px = 0; px < width; ++px) {
        const std::size_t pixel =
          static_cast<std::size_t>(py) * width + px;
        if (accepted[pixel]) {
          continue;
        }
        if (cursor[pixel] >= candidates[pixel].size()) {
          continue;
        }
        const Candidate& current = candidates[pixel][cursor[pixel]];

        const unsigned int cellX = px / 8u;
        const unsigned int cellY = py / 8u;
        const int rate = rates[cellY * cellsX + cellX];
        const int blockX = static_cast<int>(px / static_cast<unsigned>(rate))
                           * rate;
        const int blockY = static_cast<int>(py / static_cast<unsigned>(rate))
                           * rate;

        const auto key = std::make_tuple(current.primitive, blockY, blockX);
        auto found = groupIndex.find(key);
        if (found == groupIndex.end()) {
          Group group;
          group.primitive = current.primitive;
          group.blockX = blockX;
          group.blockY = blockY;
          group.rate = rate;
          group.coverageMask = 0;
          groupIndex.emplace(key, groups.size());
          groups.push_back(std::move(group));
          found = groupIndex.find(key);
        }
        Group& group = groups[found->second];
        const int i = static_cast<int>(px) - blockX;
        const int j = static_cast<int>(py) - blockY;
        group.coverageMask |=
          static_cast<std::uint32_t>(1u) << (j * rate + i);
        group.pixelX.push_back(static_cast<int>(px));
        group.pixelY.push_back(static_cast<int>(py));
        group.pixelDepth.push_back(current.depth);
      }
    }

    if (groups.empty()) {
      break;
    }

    // The masks above are frozen before any shader runs. std::map already
    // orders them by (submission index, block y, block x) as required.
    totalFrontierPixels += [&] {
      std::size_t count = 0;
      for (const Group& group : groups) {
        count += group.pixelX.size();
      }
      return count;
    }();

    for (Group& group : groups) {
      const PendingPrimitive& source =
        *std::find_if(pending_.begin(), pending_.end(),
                      [&](const PendingPrimitive& pending) {
                        return pending.index == group.primitive;
                      });

      double meanX = 0.0;
      double meanY = 0.0;
      for (std::size_t i = 0; i < group.pixelX.size(); ++i) {
        meanX += group.pixelX[i] + 0.5;
        meanY += group.pixelY[i] + 0.5;
      }
      const double invCount = 1.0 / static_cast<double>(group.pixelX.size());
      meanX *= invCount;
      meanY *= invCount;

      CoarseFragment fragment;
      fragment.primitive = group.primitive;
      fragment.round = roundCount;
      fragment.blockX = group.blockX;
      fragment.blockY = group.blockY;
      fragment.rate = group.rate;
      fragment.coverageMask = group.coverageMask;
      interpolateAttributes(source,
                            static_cast<int>(width),
                            static_cast<int>(height),
                            meanX, meanY, fragment);

      u32 colour = 0;
      const bool keep = source.shader(fragment, colour);
      ++totalShaderCalls;

      const std::uint8_t* rgba =
        reinterpret_cast<const std::uint8_t*>(&colour);
      const bool opaque = keep && rgba[3] != 0;

      for (std::size_t i = 0; i < group.pixelX.size(); ++i) {
        const int x = group.pixelX[i];
        const int y = group.pixelY[i];
        const std::size_t pixel =
          static_cast<std::size_t>(y) * width + x;
        if (opaque) {
          // One shared colour, but every pixel commits its own depth.
          target_.setPixel(static_cast<u32>(x), static_cast<u32>(y), colour);
          depth_.unsafeSet(static_cast<std::size_t>(x),
                           static_cast<std::size_t>(y),
                           group.pixelDepth[i]);
          accepted[pixel] = 1;
        }
        else {
          // Only this layer is removed; deeper candidates get their turn.
          ++cursor[pixel];
        }
      }
      if (opaque) {
        totalCommittedPixels += group.pixelX.size();
      }
    }

    ++roundCount;
  }

  captured_fragments = uniqueCandidates;
  rasterized_primitives = rasterized;
  frontier_pixels = totalFrontierPixels;
  shader_calls = totalShaderCalls;
  committed_pixels = totalCommittedPixels;
  rounds = roundCount;

  // A successful resolve consumes the batch; later batches build on the
  // target and depth just written.
  pending_.clear();
  nextSubmission_ = 0;
}

} // namespace graphics
} // namespace al
