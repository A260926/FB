#include <hpc/Parallel.hpp>
#include <hpc/Simd.hpp>
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
