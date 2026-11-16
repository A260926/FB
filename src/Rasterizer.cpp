#include <hpc/Parallel.hpp>
#include <hpc/Simd.hpp>
#include <limits>
#include <render/Rasterizer.hpp>

namespace Parallel = SoftRasterizer::Parallel;

SoftRasterizer::TraditionalRasterizer::TraditionalRasterizer(
    const std::size_t width, const std::size_t height)
    : RenderingPipeline(width, height) {}

void SoftRasterizer::TraditionalRasterizer::setSIMD(const bool enabled) {
  m_useSIMD = enabled;
}

void SoftRasterizer::TraditionalRasterizer::setBackfaceCulling(
    const bool enabled) {
  m_cullBackfaces = enabled;
}

void SoftRasterizer::TraditionalRasterizer::draw(Primitive type) {
  if (m_scenes.size() > 1) {
    throw std::invalid_argument("Rasterizer renders one scene at a time");
  }
  // Wireframes have no per-surface stochastic semantics; reject them while a
  // convergence run is active instead of silently corrupting the history.
  if (m_stochasticActive && type != Primitive::TRIANGLES) {
    throw std::invalid_argument(
        "LINES are not supported while stochastic accumulation is active");
  }
  clear();
  if (m_scenes.empty()) {
    return;
  }
  auto &scene = *m_scenes.front();
  scene.prepare();
  std::fill(m_color.begin(), m_color.end(), scene.backgroundColor());
  auto triangles = prepareScreenTriangles(scene, int(m_width), int(m_height),
                                          m_cullBackfaces, type);
  rasterizeScreenTriangles(scene, triangles, type);
}

float SoftRasterizer::TraditionalRasterizer::evaluateFrustumPlane(
    const glm::vec4 &point, FrustumPlane plane) {
  // point = projection * view * worldPosition, before dividing by w.
  // With perspectiveRH_NO, the visible clip range is -w <= x,y,z <= w.
  // After division, NDC x,y,z are in [-1,1]; depth is then mapped to [0,1].
  // Return a plane expression: >= 0 is inside/on it, < 0 is outside.
  // Zero is the plane boundary, not the minimum coordinate or a distance.
  switch (plane) {
  case FrustumPlane::Left:
    return point.w + point.x; // x >= -w -> x + w >= 0
  case FrustumPlane::Right:
    return point.w - point.x; // x <=  w -> w - x >= 0
  case FrustumPlane::Bottom:
    return point.w + point.y; // y >= -w -> y + w >= 0
  case FrustumPlane::Top:
    return point.w - point.y; // y <=  w -> w - y >= 0
  case FrustumPlane::Near:
    return point.w + point.z; // z >= -w -> z + w >= 0
  case FrustumPlane::Far:
    return point.w - point.z; // z <=  w -> w - z >= 0
  }
  throw std::invalid_argument("invalid frustum plane");
}

bool SoftRasterizer::TraditionalRasterizer::frustumCulling(
    const std::array<ClipVertex, 3> &triangle) {
  const std::array<FrustumPlane, 6> planes{
      FrustumPlane::Left, FrustumPlane::Right, FrustumPlane::Bottom,
      FrustumPlane::Top,  FrustumPlane::Near,  FrustumPlane::Far};
  for (auto plane : planes) {
    bool allOutside = true;
    for (const auto &vertex : triangle) {
      if (evaluateFrustumPlane(vertex.clip, plane) >= 0) {
        allOutside = false;
        break;
      }
    }
    if (allOutside) {
      return true;
    }
  }
  return false;
}

bool SoftRasterizer::TraditionalRasterizer::projectToScreen(
    const ClipVertex &vertex, int width, int height,
    ScreenVertex &screenVertex) {
  if (!std::isfinite(vertex.clip.w) || vertex.clip.w <= 1e-8f) {
    return false;
  }
  float inverseW = 1 / vertex.clip.w;
  auto ndc = glm::vec3(vertex.clip) * inverseW;
  if (!finite(ndc)) {
    return false;
  }
  // Scale before rounding to keep positions on a 1/256-pixel coordinate grid.
  double screenX = (ndc.x * 0.5 + 0.5) * width * SubpixelScale,
         screenY = (0.5 - ndc.y * 0.5) * height * SubpixelScale;
  // Uncut triangles can project far outside the viewport near the camera.
  if (std::abs(screenX) > MaxSubpixelCoordinate ||
      std::abs(screenY) > MaxSubpixelCoordinate) {
    return false;
  }
  screenVertex = {std::llround(screenX), std::llround(screenY), inverseW, ndc.z,
                  vertex.world};
  return true;
}

std::int64_t SoftRasterizer::TraditionalRasterizer::signedDoubleArea(
    const ScreenVertex &vertexA, const ScreenVertex &vertexB, std::int64_t x,
    std::int64_t y) {
  // cross(B-A, P-A): signed twice-area in subpixel coordinate units.
  return (vertexB.x - vertexA.x) * (y - vertexA.y) -
         (vertexB.y - vertexA.y) * (x - vertexA.x);
}

bool SoftRasterizer::TraditionalRasterizer::normalizeTriangleWinding(
    ScreenTriangle &triangle, bool cullBackfaces) {
  triangle.doubleArea =
      signedDoubleArea(triangle.vertices[0], triangle.vertices[1],
                       triangle.vertices[2].x, triangle.vertices[2].y);
  if (triangle.doubleArea == 0) {
    return false;
  }
  // Viewport Y points down, so a front-facing triangle has negative area.
  triangle.frontFace = triangle.doubleArea < 0;
  if (cullBackfaces && !triangle.frontFace) {
    return false;
  }
  if (triangle.doubleArea < 0) {
    std::swap(triangle.vertices[1], triangle.vertices[2]);
    triangle.doubleArea = -triangle.doubleArea;
  }
  return true;
}

void SoftRasterizer::TraditionalRasterizer::prepareTriangleEdges(
    ScreenTriangle &triangle, Primitive type) {
  if (type == Primitive::TRIANGLES) {
    return;
  }
  // Visit all three directed edges, each opposite vertex j:
  // j=0: 1->2, j=1: 2->0, j=2: 0->1 (zero-based vertex indices).
  for (int j = 0; j < 3; ++j) {
    const auto &edgeStart = triangle.vertices[(j + 1) % 3],
               &edgeEnd = triangle.vertices[(j + 2) % 3];
    // hypot gives subpixel edge length; multiplying by SubpixelScale converts
    // pixel distance to twice-area units, scaled by SubpixelScale^2.
    triangle.wireframeDistanceScale[j] =
        float(std::hypot(double(edgeEnd.x - edgeStart.x),
                         double(edgeEnd.y - edgeStart.y)) *
              SubpixelScale);
  }
}

bool SoftRasterizer::TraditionalRasterizer::calculateTrianglePixelBounds(
    ScreenTriangle &triangle, int width, int height) {
  auto minX = std::min({triangle.vertices[0].x, triangle.vertices[1].x,
                        triangle.vertices[2].x}),
       maxX = std::max({triangle.vertices[0].x, triangle.vertices[1].x,
                        triangle.vertices[2].x});
  auto minY = std::min({triangle.vertices[0].y, triangle.vertices[1].y,
                        triangle.vertices[2].y}),
       maxY = std::max({triangle.vertices[0].y, triangle.vertices[1].y,
                        triangle.vertices[2].y});
  // Bounds include only pixel centers, then clamp to the framebuffer.
  triangle.startX =
      std::max(0, int(std::ceil(double(minX) / SubpixelScale - 0.5)));
  triangle.endX =
      std::min(width - 1, int(std::floor(double(maxX) / SubpixelScale - 0.5)));
  triangle.startY =
      std::max(0, int(std::ceil(double(minY) / SubpixelScale - 0.5)));
  triangle.endY =
      std::min(height - 1, int(std::floor(double(maxY) / SubpixelScale - 0.5)));
  return triangle.startX <= triangle.endX && triangle.startY <= triangle.endY;
}

bool SoftRasterizer::TraditionalRasterizer::prepareScreenTriangle(
    const std::array<ClipVertex, 3> &clipVertices, const RasterTriangle &source,
    int width, int height, bool cullBackfaces, Primitive type,
    ScreenTriangle &triangle) {
  triangle.material = source.material;
  triangle.shader = source.shader;
  triangle.surfaceId = source.surfaceId;
  for (int j = 0; j < 3; ++j) {
    if (!projectToScreen(clipVertices[j], width, height,
                         triangle.vertices[j])) {
      return false;
    }
  }
  if (!normalizeTriangleWinding(triangle, cullBackfaces)) {
    return false;
  }
  prepareTriangleEdges(triangle, type);
  return calculateTrianglePixelBounds(triangle, width, height);
}

std::vector<SoftRasterizer::TraditionalRasterizer::ScreenTriangle>
SoftRasterizer::TraditionalRasterizer::prepareScreenTriangles(
    const Scene &scene, int width, int height, bool cullBackfaces,
    Primitive type) {
  const auto viewProjection =
      scene.getCamera().projectionMatrix(float(width) / height) *
      scene.getCamera().view();
  std::vector<ScreenTriangle> result;
  for (const auto &worldTriangle : scene.triangles()) {
    if (worldTriangle.shader &&
        (worldTriangle.shader->type == SHADERS_TYPE::BUMP ||
         worldTriangle.shader->type == SHADERS_TYPE::DISPLACEMENT)) {
      throw std::invalid_argument("unsupported raster shader");
    }
    std::array<ClipVertex, 3> clipVertices;
    for (int i = 0; i < 3; ++i) {
      const auto &vertex = worldTriangle.vertices[i];
      clipVertices[i] = {viewProjection * glm::vec4(vertex.position, 1),
                         vertex};
    }
    if (frustumCulling(clipVertices)) {
      continue;
    }
    ScreenTriangle triangle{};
    if (prepareScreenTriangle(clipVertices, worldTriangle, width, height,
                              cullBackfaces, type, triangle)) {
      result.push_back(std::move(triangle));
    }
  }
  return result;
}

std::vector<std::vector<std::size_t>>
SoftRasterizer::TraditionalRasterizer::binTrianglesIntoTiles(
    const std::vector<ScreenTriangle> &triangles, int tileColumns,
    int tileRows) {
  std::vector<std::vector<std::size_t>> tileTriangles(std::size_t(tileColumns) *
                                                      tileRows);
  for (std::size_t i = 0; i < triangles.size(); ++i) {
    const auto &triangle = triangles[i];
    for (int y = triangle.startY / TileSize; y <= triangle.endY / TileSize;
         ++y) {
      for (int x = triangle.startX / TileSize; x <= triangle.endX / TileSize;
           ++x) {
        tileTriangles[std::size_t(y) * tileColumns + x].push_back(i);
      }
    }
  }
  return tileTriangles;
}

void SoftRasterizer::TraditionalRasterizer::rasterizeScreenTriangles(
    const Scene &scene, const std::vector<ScreenTriangle> &triangles,
    Primitive type) {
  const int tileColumns = (int(m_width) + TileSize - 1) / TileSize,
            tileRows = (int(m_height) + TileSize - 1) / TileSize;
  auto tileTriangles = binTrianglesIntoTiles(triangles, tileColumns, tileRows);
  /*Bin screen triangles first; tasks own disjoint framebuffer tiles.*/
  Parallel::parallelFor(
      std::size_t(0), tileTriangles.size(), [&](std::size_t tile) {
        int tileX = int(tile % tileColumns) * TileSize,
            tileY = int(tile / tileColumns) * TileSize;
        for (auto index : tileTriangles[tile]) {
          rasterizeTriangleInTile(scene, triangles[index], tileX, tileY, type);
        }
      });
}

bool SoftRasterizer::TraditionalRasterizer::calculatePixelCoverage(
    const ScreenTriangle &triangle, int x, int y, Primitive type,
    glm::vec3 &screenWeights) {
  // One sample at the original pixel center: (x + 0.5, y + 0.5) * scale.
  // increase accuracy by using subpixel coordinates, then divide by
  // SubpixelScale^2.
  auto pixelX = std::int64_t(x) * SubpixelScale + SubpixelScale / 2,
       pixelY = std::int64_t(y) * SubpixelScale + SubpixelScale / 2;
  // Subtriangle/whole-triangle area ratios give screen barycentrics.
  std::array<std::int64_t, 3> subtriangleDoubleAreas{

      signedDoubleArea(triangle.vertices[1], triangle.vertices[2], pixelX,
                       pixelY),
      signedDoubleArea(triangle.vertices[2], triangle.vertices[0], pixelX,
                       pixelY),
      signedDoubleArea(triangle.vertices[0], triangle.vertices[1], pixelX,
                       pixelY)};

  bool inside = true, wire = false;
  for (int j = 0; j < 3; ++j) {
    // Include every edge and vertex: all three signed areas must be >= 0.
    inside &= subtriangleDoubleAreas[j] >= 0;
    if (type != Primitive::TRIANGLES) {
      // Twice-area / edge length is perpendicular distance to the edge.
      // With the stored scaling, 0.8 is a wireframe distance in pixels.
      wire |= float(subtriangleDoubleAreas[j]) <
              0.8f * triangle.wireframeDistanceScale[j];
    }
    screenWeights[j] =
        float(subtriangleDoubleAreas[j]) / float(triangle.doubleArea);
  }
  return inside && (type == Primitive::TRIANGLES || wire);
}

/*Rasterize a triangle inside the current tile.*/
void SoftRasterizer::TraditionalRasterizer::rasterizeTriangleInTile(
    const Scene &scene, const ScreenTriangle &triangle, const int tileX,
    const int tileY, Primitive type) {
  const int startX = std::max(triangle.startX, tileX),
            endX = std::min(triangle.endX, tileX + TileSize - 1);
  const int startY = std::max(triangle.startY, tileY),
            endY = std::min(triangle.endY, tileY + TileSize - 1);
  for (int y = startY; y <= endY; ++y) {
    for (int x = startX; x <= endX;) {
      int batchSize = m_useSIMD ? std::min(8, endX - x + 1) : 1;
      float barycentric[3][8]{}, depths[8]{};
      bool covered[8]{};
      for (int lane = 0; lane < batchSize; ++lane) {
        glm::vec3 screenWeights;
        covered[lane] =
            calculatePixelCoverage(triangle, x + lane, y, type, screenWeights);
        for (int j = 0; j < 3; ++j) {
          barycentric[j][lane] = screenWeights[j];
        }
      }
      if (m_useSIMD && batchSize == 8) {
        interpolateDepthAndWeightsSIMD(triangle, barycentric, depths);
      } else {
        interpolateDepthAndWeightsScalar(triangle, barycentric, depths,
                                         batchSize);
      }
      shadePixelBatch(scene, triangle, x, y, barycentric, depths, covered,
                      batchSize);
      x += batchSize;
    }
  }
}

/*Eight-pixel perspective interpolation through portable SIMDe.*/
void SoftRasterizer::TraditionalRasterizer::interpolateDepthAndWeightsSIMD(
    const ScreenTriangle &triangle, float (&barycentric)[3][8],
    float (&depths)[8]) {

  // Coverage stays exact in 64-bit fixed point; interpolate eight
  // pixels per batch.
  auto b0 = simde_mm256_loadu_ps(barycentric[0]),
       b1 = simde_mm256_loadu_ps(barycentric[1]),
       b2 = simde_mm256_loadu_ps(barycentric[2]);
  auto mul = [](Float8 value, float scale) {
    return simde_mm256_mul_ps(value, simde_mm256_set1_ps(scale));
  };
  auto z =
      simde_mm256_add_ps(simde_mm256_add_ps(mul(b0, triangle.vertices[0].z),
                                            mul(b1, triangle.vertices[1].z)),
                         mul(b2, triangle.vertices[2].z));
  simde_mm256_storeu_ps(
      depths, simde_mm256_add_ps(mul(z, 0.5f), simde_mm256_set1_ps(0.5f)));
  b0 = mul(b0, triangle.vertices[0].inverseW);
  b1 = mul(b1, triangle.vertices[1].inverseW);
  b2 = mul(b2, triangle.vertices[2].inverseW);
  auto denominator = simde_mm256_add_ps(simde_mm256_add_ps(b0, b1), b2);
  simde_mm256_storeu_ps(barycentric[0], simde_mm256_div_ps(b0, denominator));
  simde_mm256_storeu_ps(barycentric[1], simde_mm256_div_ps(b1, denominator));
  simde_mm256_storeu_ps(barycentric[2], simde_mm256_div_ps(b2, denominator));
}

/*Scalar interpolation, also used for the tail of a SIMD batch.*/
void SoftRasterizer::TraditionalRasterizer::interpolateDepthAndWeightsScalar(
    const ScreenTriangle &triangle, float (&barycentric)[3][8],
    float (&depths)[8], const int batchSize) {
  for (int lane = 0; lane < batchSize; ++lane) {
    depths[lane] = ((barycentric[0][lane] * triangle.vertices[0].z +
                     barycentric[1][lane] * triangle.vertices[1].z) +
                    barycentric[2][lane] * triangle.vertices[2].z) *
                       0.5f +
                   0.5f;
    for (int j = 0; j < 3; ++j) {
      barycentric[j][lane] *= triangle.vertices[j].inverseW;
    }
    float denominator =
        (barycentric[0][lane] + barycentric[1][lane]) + barycentric[2][lane];
    for (int j = 0; j < 3; ++j) {
      barycentric[j][lane] /= denominator;
    }
  }
}

void SoftRasterizer::TraditionalRasterizer::shadePixelBatch(
    const Scene &scene, const ScreenTriangle &triangle, int x, int y,
    const float (&perspectiveWeights)[3][8], const float (&depths)[8],
    const bool (&covered)[8], int batchSize) {
  for (int lane = 0; lane < batchSize; ++lane) {
    // Near/far bounds are checked per pixel because triangles stay whole.
    if (!covered[lane] || !(depths[lane] >= 0 && depths[lane] <= 1)) {
      continue;
    }
    auto pixel = std::size_t(y) * m_width + x + lane;
    if (depths[lane] < m_depth[pixel]) {
      m_depth[pixel] = depths[lane];
      m_color[pixel] = shadeFragment(scene, triangle,
                                     {perspectiveWeights[0][lane],
                                      perspectiveWeights[1][lane],
                                      perspectiveWeights[2][lane]});
    }
  }
}

SoftRasterizer::Vertex
SoftRasterizer::TraditionalRasterizer::interpolateFragment(
    const ScreenTriangle &triangle, const glm::vec3 &perspectiveWeights) {
  const auto &vertices = triangle.vertices;
  Vertex fragment;
  fragment.position = perspectiveWeights.x * vertices[0].world.position +
                      perspectiveWeights.y * vertices[1].world.position +
                      perspectiveWeights.z * vertices[2].world.position;
  fragment.normal = unit(perspectiveWeights.x * vertices[0].world.normal +
                         perspectiveWeights.y * vertices[1].world.normal +
                         perspectiveWeights.z * vertices[2].world.normal);
  fragment.texCoord = perspectiveWeights.x * vertices[0].world.texCoord +
                      perspectiveWeights.y * vertices[1].world.texCoord +
                      perspectiveWeights.z * vertices[2].world.texCoord;
  fragment.color = perspectiveWeights.x * vertices[0].world.color +
                   perspectiveWeights.y * vertices[1].world.color +
                   perspectiveWeights.z * vertices[2].world.color;
  return fragment;
}

glm::vec3 SoftRasterizer::TraditionalRasterizer::shadeBlinnPhong(
    const Scene &scene, const ScreenTriangle &triangle, const Vertex &fragment,
    const glm::vec3 &albedo) {
  auto normal = fragment.normal;
  auto view = unit(scene.getCamera().position() - fragment.position);
  if (glm::dot(normal, view) < 0) {
    normal = -normal;
  }
  glm::vec3 result =
      0.025f * albedo +
      (triangle.frontFace ? triangle.material->emission : glm::vec3(0));
  for (const auto &[name, light] : scene.pointLights()) {
    auto lightDirection = light->position - fragment.position;
    float distance2 = glm::dot(lightDirection, lightDirection);
    if (distance2 <= 1e-12f) {
      continue;
    }
    auto wi = lightDirection / std::sqrt(distance2);
    float cosine = std::max(0.f, glm::dot(normal, wi));
    if (cosine <= 0) {
      continue;
    }
    auto halfway = unit(wi + view, normal);
    auto brdf = albedo * (cosine / Pi) +
                triangle.material->Ks *
                    std::pow(std::max(0.f, glm::dot(normal, halfway)),
                             triangle.material->specularExponent);
    result += brdf * light->intensity / distance2;
  }
  return result;
}

glm::vec3 SoftRasterizer::TraditionalRasterizer::shadeFragment(
    const Scene &scene, const ScreenTriangle &triangle,
    const glm::vec3 &perspectiveWeights) {
  auto fragment = interpolateFragment(triangle, perspectiveWeights);
  auto type = triangle.shader ? triangle.shader->type : SHADERS_TYPE::PHONG;
  if (type == SHADERS_TYPE::NORMAL) {
    return (fragment.normal + glm::vec3(1)) * 0.5f;
  }
  auto albedo = triangle.material->albedo(fragment.texCoord) *
                glm::clamp(fragment.color, glm::vec3(0), glm::vec3(1));
  if (type == SHADERS_TYPE::TEXTURE) {
    return albedo;
  }
  return shadeBlinnPhong(scene, triangle, fragment, albedo);
}

namespace {
// Reject a frame request before any sample buffer or accumulator is touched.
void validateStochasticFrameRequest(bool configured,
                                    std::uint64_t accumulatedFrames,
                                    std::uint64_t firstFrame,
                                    std::uint64_t count) {
  if (!configured) {
    throw std::invalid_argument(
        "stochastic advance requires configureStochastic() first");
  }
  if (count == 0) {
    throw std::invalid_argument("stochastic advance count must be nonzero");
  }
  if (firstFrame != accumulatedFrames) {
    throw std::invalid_argument(
        "stochastic frames must continue from frame 0 without repeats or "
        "gaps");
  }
  if (firstFrame > std::numeric_limits<std::uint64_t>::max() - (count - 1)) {
    throw std::invalid_argument("stochastic frame range overflow");
  }
}
} // namespace

void SoftRasterizer::TraditionalRasterizer::configureStochastic(
    const StochasticSettings &settings) {
  // Every uint64_t seed/generation pair is a finite, legal stream selector.
  // Reconfiguring starts a fresh run, so previous history cannot leak in.
  const std::size_t pixels = m_width * m_height;
  m_stochasticSettings = settings;
  m_stochasticConfigured = true;
  m_stochasticActive = false;
  m_stochasticFrames = 0;
  m_stochasticMean.assign(pixels, glm::vec3(0));
  m_stochasticDepth.assign(pixels * StochasticSampleCount,
                           std::numeric_limits<float>::infinity());
  m_stochasticSurface.assign(pixels * StochasticSampleCount, 0);
  m_stochasticColor.assign(pixels * StochasticSampleCount, glm::vec3(0));
}

void SoftRasterizer::TraditionalRasterizer::resetStochastic() {
  m_stochasticActive = false;
  m_stochasticFrames = 0;
  std::fill(m_stochasticMean.begin(), m_stochasticMean.end(), glm::vec3(0));
  std::fill(m_stochasticDepth.begin(), m_stochasticDepth.end(),
            std::numeric_limits<float>::infinity());
  std::fill(m_stochasticSurface.begin(), m_stochasticSurface.end(), 0);
  std::fill(m_stochasticColor.begin(), m_stochasticColor.end(), glm::vec3(0));
  // The ordinary framebuffers keep no stochastic history either.
  clear();
}

void SoftRasterizer::TraditionalRasterizer::advance(
    const std::uint64_t firstFrame, const std::uint64_t count) {
  // Validate everything first: an illegal request must leave both the image
  // and the accumulated history untouched.
  validateStochasticFrameRequest(m_stochasticConfigured, m_stochasticFrames,
                                 firstFrame, count);
  if (m_scenes.size() > 1) {
    throw std::invalid_argument("Rasterizer renders one scene at a time");
  }
  if (m_scenes.empty()) {
    throw std::invalid_argument("stochastic advance requires a scene");
  }
  auto &scene = *m_scenes.front();
  // prepare() rebuilds the cached triangle stream and rejects zero/duplicate
  // surface ids before any framebuffer write below.
  scene.prepare();
  for (const auto &triangle : scene.triangles()) {
    if (!std::isfinite(triangle.material->opacity) ||
        triangle.material->opacity < 0.f ||
        triangle.material->opacity > 1.f) {
      throw std::invalid_argument("opacity must be finite and in [0, 1]");
    }
  }
  auto screenTriangles = prepareScreenTriangles(
      scene, int(m_width), int(m_height), m_cullBackfaces,
      Primitive::TRIANGLES);
  for (std::uint64_t offset = 0; offset < count; ++offset) {
    rasterizeStochasticFrame(scene, screenTriangles, firstFrame + offset);
  }
}

void SoftRasterizer::TraditionalRasterizer::resolveStochastic() {
  if (!m_stochasticActive) {
    return;
  }
  // Read-only: re-publish the running mean without touching accumulators or
  // the per-frame sample buffers.
  std::copy(m_stochasticMean.begin(), m_stochasticMean.end(), m_color.begin());
}

void SoftRasterizer::TraditionalRasterizer::rasterizeStochasticFrame(
    const Scene &scene, const std::vector<ScreenTriangle> &triangles,
    const std::uint64_t frame) {
  const std::size_t pixels = m_width * m_height;
  const auto background = scene.backgroundColor();
  // Every frame restarts the eight samples at background / infinity; only the
  // running mean carries across frames.
  std::fill(m_stochasticDepth.begin(), m_stochasticDepth.end(),
            std::numeric_limits<float>::infinity());
  std::fill(m_stochasticSurface.begin(), m_stochasticSurface.end(), 0);
  for (auto &color : m_stochasticColor) {
    color = background;
  }

  const int tileColumns = (int(m_width) + TileSize - 1) / TileSize,
            tileRows = (int(m_height) + TileSize - 1) / TileSize;
  const auto tileTriangles =
      binTrianglesIntoTiles(triangles, tileColumns, tileRows);
  const auto settings = m_stochasticSettings;
  /*Tiles own disjoint pixels, and every decision is a pure function of the
   * deterministic hash and the fragment depth/id, so scheduling cannot change
   * the result.*/
  Parallel::parallelFor(
      std::size_t(0), tileTriangles.size(), [&](std::size_t tile) {
        const int tileX = int(tile % tileColumns) * TileSize,
                  tileY = int(tile / tileColumns) * TileSize;
        for (const auto index : tileTriangles[tile]) {
          const auto &triangle = triangles[index];
          const int startX = std::max(triangle.startX, tileX),
                    endX = std::min(triangle.endX, tileX + TileSize - 1);
          const int startY = std::max(triangle.startY, tileY),
                    endY = std::min(triangle.endY, tileY + TileSize - 1);
          for (int y = startY; y <= endY; ++y) {
            for (int x = startX; x <= endX; ++x) {
              // One attribute sample at the original pixel center, exactly as
              // the opaque path uses.
              glm::vec3 screenWeights;
              if (!calculatePixelCoverage(triangle, x, y,
                                          Primitive::TRIANGLES,
                                          screenWeights)) {
                continue;
              }
              float barycentric[3][8]{};
              float depths[8]{};
              for (int j = 0; j < 3; ++j) {
                barycentric[j][0] = screenWeights[j];
              }
              interpolateDepthAndWeightsScalar(triangle, barycentric, depths,
                                               1);
              const float depth = depths[0];
              // Near/far depth range rule is shared with the opaque path.
              if (!(depth >= 0 && depth <= 1)) {
                continue;
              }
              const float opacity = triangle.material->opacity;
              if (opacity <= 0.f) {
                continue; // Fully transparent surfaces never contribute.
              }
              // Shade once per covered pixel: real TEXTURE/BLINN_PHONG rules
              // and perspective-correct interpolation, un-premultiplied.
              const glm::vec3 fragmentColor = shadeFragment(
                  scene, triangle,
                  {barycentric[0][0], barycentric[1][0], barycentric[2][0]});
              const std::size_t pixel = std::size_t(y) * m_width + x;
              for (int sample = 0; sample < StochasticSampleCount;
                   ++sample) {
                if (opacity < 1.f) {
                  const float random = stochasticHashRandom(
                      settings.seed, settings.generation, frame,
                      std::uint64_t(x), std::uint64_t(y),
                      std::uint64_t(sample), triangle.surfaceId);
                  // A rejected fragment must not write depth or color.
                  if (random >= opacity) {
                    continue;
                  }
                }
                const std::size_t slot =
                    pixel * StochasticSampleCount + sample;
                const bool nearer = depth < m_stochasticDepth[slot];
                const bool sameDepthSmallerId =
                    depth == m_stochasticDepth[slot] &&
                    triangle.surfaceId < m_stochasticSurface[slot];
                if (nearer || sameDepthSmallerId) {
                  m_stochasticDepth[slot] = depth;
                  m_stochasticSurface[slot] = triangle.surfaceId;
                  m_stochasticColor[slot] = fragmentColor;
                }
              }
            }
          }
        }
      });

  // Average the eight samples, then fold the frame mean into the online
  // running mean. Welford-style delta update keeps only one accumulator per
  // pixel regardless of the frame count.
  m_stochasticFrames += 1;
  const float inverseSamples = 1.f / float(StochasticSampleCount);
  const float inverseFrames = 1.f / float(m_stochasticFrames);
  for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
    glm::vec3 frameMean(0);
    for (int sample = 0; sample < StochasticSampleCount; ++sample) {
      frameMean += m_stochasticColor[pixel * StochasticSampleCount + sample];
    }
    frameMean *= inverseSamples;
    m_stochasticMean[pixel] +=
        (frameMean - m_stochasticMean[pixel]) * inverseFrames;
    // resolve() only ever reads the current mean through pixels().
    m_color[pixel] = m_stochasticMean[pixel];
  }
  m_stochasticActive = true;
}

float SoftRasterizer::TraditionalRasterizer::stochasticSampleDepth(
    const std::size_t pixel, const int sample) const {
  if (pixel >= m_width * m_height || sample < 0 ||
      sample >= StochasticSampleCount) {
    throw std::invalid_argument("invalid stochastic sample coordinate");
  }
  return m_stochasticDepth[pixel * StochasticSampleCount + sample];
}

std::uint64_t SoftRasterizer::TraditionalRasterizer::stochasticSampleSurface(
    const std::size_t pixel, const int sample) const {
  if (pixel >= m_width * m_height || sample < 0 ||
      sample >= StochasticSampleCount) {
    throw std::invalid_argument("invalid stochastic sample coordinate");
  }
  return m_stochasticSurface[pixel * StochasticSampleCount + sample];
}
