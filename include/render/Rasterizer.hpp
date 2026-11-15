#pragma once
#include <base/Render.hpp>

namespace SoftRasterizer {

// Traditional Rasterizer
class TraditionalRasterizer : public RenderingPipeline {
public:
  explicit TraditionalRasterizer(const std::size_t width = 800,
                                 const std::size_t height = 600);
  void setSIMD(const bool enabled);
  void setBackfaceCulling(const bool enabled);
  void draw(Primitive type = Primitive::TRIANGLES) override;

private:
  enum class FrustumPlane { Left, Right, Bottom, Top, Near, Far };

  /*Clip coordinates are temporary; world attributes remain unchanged.*/
  struct ClipVertex {
    glm::vec4 clip; // (x, y, z, w) after projection, before perspective divide.
    Vertex world;
  };

  struct ScreenVertex {
    std::int64_t x, y; // Screen coordinates in subpixel units.
    float inverseW, z; // 1/clip.w and NDC depth clip.z/clip.w.
    Vertex world;
  };

  struct ScreenTriangle {
    std::array<ScreenVertex, 3> vertices;
    std::int64_t doubleArea; // Positive after winding normalization; no /2.
    // Wireframe only: twice-area units per pixel of distance from an edge.
    std::array<float, 3> wireframeDistanceScale;
    int startX, startY, endX, endY;
    bool frontFace;
    std::shared_ptr<Material> material;
    std::shared_ptr<Shader> shader;
  };

  /*Clip-space plane expression before /w: >= 0 is inside, < 0 is outside.*/
  static float evaluateFrustumPlane(const glm::vec4 &point, FrustumPlane plane);
  /*Cull only if all three vertices are outside the same frustum plane.*/
  static bool frustumCulling(const std::array<ClipVertex, 3> &triangle);

  /*Perspective divide, viewport mapping and screen triangle setup.*/
  static bool projectToScreen(const ClipVertex &vertex, int width, int height,
                              ScreenVertex &screenVertex);
  static bool normalizeTriangleWinding(ScreenTriangle &triangle,
                                       bool cullBackfaces);
  static void prepareTriangleEdges(ScreenTriangle &triangle, Primitive type);
  static bool calculateTrianglePixelBounds(ScreenTriangle &triangle, int width,
                                           int height);
  static bool
  prepareScreenTriangle(const std::array<ClipVertex, 3> &clipVertices,
                        const RasterTriangle &source, int width, int height,
                        bool cullBackfaces, Primitive type,
                        ScreenTriangle &triangle);

  /*Signed 2*area(A,B,P); its sign also tells which side of AB contains P.*/
  static std::int64_t signedDoubleArea(const ScreenVertex &a,
                                       const ScreenVertex &b, std::int64_t x,
                                       std::int64_t y);

  /*Cull, project and keep one screen triangle per original triangle.*/
  static std::vector<ScreenTriangle>
  prepareScreenTriangles(const Scene &scene, int width, int height,
                         bool cullBackfaces, Primitive type);

  /*Assign triangles to tiles; each task owns a disjoint pixel region.*/
  static std::vector<std::vector<std::size_t>>
  binTrianglesIntoTiles(const std::vector<ScreenTriangle> &triangles,
                        int tileColumns, int tileRows);
  void rasterizeScreenTriangles(const Scene &scene,
                                const std::vector<ScreenTriangle> &triangles,
                                Primitive type);

  /*Test coverage and depth, then shade pixels inside the current tile.*/
  static bool calculatePixelCoverage(const ScreenTriangle &triangle, int x,
                                     int y, Primitive type,
                                     glm::vec3 &screenWeights);
  void rasterizeTriangleInTile(const Scene &scene,
                               const ScreenTriangle &triangle, const int tileX,
                               const int tileY, Primitive type);
  void shadePixelBatch(const Scene &scene, const ScreenTriangle &triangle,
                       int x, int y, const float (&perspectiveWeights)[3][8],
                       const float (&depths)[8], const bool (&covered)[8],
                       int batchSize);
  /*Screen barycentric input -> depth and perspective-correct weights output.*/
  static void interpolateDepthAndWeightsSIMD(const ScreenTriangle &triangle,
                                             float (&barycentric)[3][8],
                                             float (&depths)[8]);
  static void interpolateDepthAndWeightsScalar(const ScreenTriangle &triangle,
                                               float (&barycentric)[3][8],
                                               float (&depths)[8],
                                               const int batchSize);

  /*Perspective weights interpolate world-space fragment attributes.*/
  static Vertex interpolateFragment(const ScreenTriangle &triangle,
                                    const glm::vec3 &perspectiveWeights);
  static glm::vec3 shadeBlinnPhong(const Scene &scene,
                                   const ScreenTriangle &triangle,
                                   const Vertex &fragment,
                                   const glm::vec3 &albedo);
  static glm::vec3 shadeFragment(const Scene &scene,
                                 const ScreenTriangle &triangle,
                                 const glm::vec3 &perspectiveWeights);

private:
  // Chosen fixed-point precision, not a mandatory rasterization standard.
  // Each axis has 256 coordinate steps per pixel (8 fractional bits).
  // Store round(pixelPosition * 256): resolution and sample count stay the
  // same. This stabilizes integer edge tests; it is not MSAA or extra rendered
  // pixels.
  static constexpr std::int64_t SubpixelScale = 256;
  // Keep products in signedDoubleArea within the int64_t range.
  static constexpr std::int64_t MaxSubpixelCoordinate = 1LL << 29;
  static constexpr int TileSize = 32;
  bool m_useSIMD = true;
  bool m_cullBackfaces = false;
};
} // namespace SoftRasterizer
