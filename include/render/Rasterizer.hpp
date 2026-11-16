#pragma once
#include <base/Render.hpp>
#include <render/Stochastic.hpp>

namespace SoftRasterizer {

// Traditional Rasterizer
class TraditionalRasterizer : public RenderingPipeline {
public:
  explicit TraditionalRasterizer(const std::size_t width = 800,
                                 const std::size_t height = 600);
  void setSIMD(const bool enabled);
  void setBackfaceCulling(const bool enabled);
  void draw(Primitive type = Primitive::TRIANGLES) override;

  /*Stochastic transparency progression.*/
  // Begin (or re-begin after resetStochastic) a convergence run. The settings
  // only select the deterministic random stream; nothing is rendered yet.
  void configureStochastic(const StochasticSettings &settings);
  // Drop every accumulated frame so a new run can start; configuration stays
  // valid until the next configureStochastic() call.
  void resetStochastic();
  bool stochasticActive() const {
    return m_stochasticActive;
  }
  // Number of frames actually folded into the current running mean.
  std::uint64_t accumulatedFrames() const {
    return m_stochasticFrames;
  }
  StochasticSettings stochasticSettings() const {
    return m_stochasticSettings;
  }
  // Render and accumulate `count` consecutive frames starting at firstFrame.
  // Frame numbering starts at zero and must continue the previous run without
  // repeats or gaps. Invalid arguments are rejected before any sample or
  // accumulator is touched. LINES are not supported while a run is active.
  void advance(std::uint64_t firstFrame, std::uint64_t count = 1);
  // Re-publish the current running mean to the color buffer. Read-only with
  // respect to the accumulated history, so repeated calls are identical.
  void resolveStochastic();
  // Inspection of the latest frame's eight per-pixel samples for debugging.
  float stochasticSampleDepth(std::size_t pixel, int sample) const;
  std::uint64_t stochasticSampleSurface(std::size_t pixel, int sample) const;

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
    // Stable source identity used as the stochastic hash's surface dimension.
    std::uint64_t surfaceId = 0;
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

  /*One stochastic-transparency frame: eight samples per pixel, then fold the
   * frame mean into the persistent running mean.*/
  void rasterizeStochasticFrame(const Scene &scene,
                                const std::vector<ScreenTriangle> &triangles,
                                std::uint64_t frame);

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

  /*Stochastic-transparency state. All persistent buffers are sized by the
   * pixel count only; no transparent fragment lists are retained.*/
  bool m_stochasticConfigured = false;
  bool m_stochasticActive = false;
  StochasticSettings m_stochasticSettings{};
  std::uint64_t m_stochasticFrames = 0; // Frames folded into the running mean.
  // Running mean of per-frame means, updated online per accumulated frame.
  std::vector<glm::vec3> m_stochasticMean;
  // Latest frame's eight independent samples: depth, surface identity and
  // un-premultiplied linear color. Background color and infinity initially.
  std::vector<float> m_stochasticDepth;
  std::vector<std::uint64_t> m_stochasticSurface;
  std::vector<glm::vec3> m_stochasticColor;
};
} // namespace SoftRasterizer
