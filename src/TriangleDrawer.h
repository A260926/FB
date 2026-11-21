#ifndef ANDREW_LIM_GRAPHICS_TRIANGLE_DRAWER_H
#define ANDREW_LIM_GRAPHICS_TRIANGLE_DRAWER_H
#include "Triangle.h"
#include "ImageData.h"
#include "Graphics3D.h"
#include "Grid.h"
#include "Vertex.h"
#include "Vector3f.h"
#include <cstdint>
#include <vector>

typedef Grid<double> ZBuffer;

namespace al { namespace graphics {

/*
Records the fragment that won the depth test at a pixel. Only populated when
TriangleDrawer::recordFragments is enabled (default off).
*/
struct FragmentRecord
{
  std::uint32_t color;
  // Perspective-correct float attribute carried through the rgb channel.
  // Callers choose what it means (e.g. local position for capture, float
  // color for reprojection).
  Vector3f attribute;
  // Perspective-correct unit local-space normal (from the normal channel).
  Vector3f normal;
  // Screen-space barycentric weights of the winning fragment and the
  // caller-supplied triangle id (Triangle::textureID).
  float bary[3];
  int triangleId;
  bool valid;
  FragmentRecord()
  : color(0), attribute(), normal(), bary{0.0f, 0.0f, 0.0f}, triangleId(-1),
    valid(false)
  {}
};

class TriangleDrawer
{
public:
  enum DrawMode {
    DRAW_PERSPECTIVE, DRAW_SOLID, DRAW_WIREFRAME, DRAW_AFFINE,
    DRAW_RGB_SHADED
  };

  enum Algo {
    SCANLINE, BARYCENTRIC, BARYCENTRIC_O1, BARYCENTRIC_O2
  };

  enum LightsStyle {
    LIGHTS_STYLE_NONE, LIGHTS_STYLE_FLAT, LIGHTS_STYLE_SMOOTH
  };
  Algo algo = SCANLINE;
  DrawMode drawMode;
  LightsStyle lightsStyle;
  Vector3f lightDirection;
  ZBuffer* depthBuffer;
  std::uint32_t color;
  ImageData* textureImageData;

  // Opt-in extensions used by the depth impostor. Both default to off, so the
  // existing algorithms and drawing paths are unaffected.
  //
  // When true, the fragment depth written/compared is the true projected
  // depth from the clip-space z (vertex pos.z), instead of the legacy
  // inverse-w value. The vertex pos must already be in NDC (post
  // perspective-divide) with pos.z divided by w as well.
  bool useProjectedDepth = false;
  // When true, the winning fragment's color, local position and unit normal
  // are written to fragmentRecords in the same final depth commit that
  // updates the color/depth targets.
  bool recordFragments = false;
  std::vector<FragmentRecord>* fragmentRecords = nullptr;
  // When true, barycentric coverage accepts either triangle winding, so a
  // triangle is drawn from both sides. Defaults to false (back-face coverage
  // of the existing algorithms is preserved).
  bool doubleSided = false;

  TriangleDrawer(ZBuffer* depthBuffer = 0)
  : drawMode(TriangleDrawer::DRAW_SOLID)
  , lightsStyle(TriangleDrawer::LIGHTS_STYLE_SMOOTH)
  , lightDirection(1.0f, 0.0f, 1.0f)
  , depthBuffer(depthBuffer)
  , color(0)
  , textureImageData(0)
  {
    lightDirection = G3D::normalize(lightDirection);
  }

  void wireframeTriangle(ImageData& imageData,
                         const Triangle& triangle,
                         u32 rgba);

  // Delegates drawing to the other triangle drawing methods
  void triangle(ImageData& imageData, const Triangle& triangle);

  // Standard scanline approach that uses slope and top/bottom halves
  void scanlineTriangle(ImageData& imageData, const Triangle& triangle);

  // Simple unoptimized barycentric approach
  // Checks every pixel in the bounding box
  void barycentricTriangle(ImageData& imageData, const Triangle& triangle);

  // Optimized barycentric approach that uses interpolates using additions
  // Based on the following links:
  // https://fgiesen.wordpress.com/2013/02/10/optimizing-the-basic-rasterizer/
  // https://github.com/gustavopezzi/triangle-rasterizer-float
  void barycentricO1(ImageData& imageData, const Triangle& triangle);

  // Similar to barycentricO1 but also reduces checks on pixels that lie on the
  // outside of the triangle.
  // First the triangle is broken into 2 smaller triangles - a flat-bottom and
  // flat-top. Then for each triangle 2 main things happen:
  // 1. While scanning a row horizontally starting from the left most vertex,
  //    the x-coordinate of the first pixel found inside the triangle becomes
  //    the starting x for checking the next row.
  // 2. While inside the triangle, stop scanning the current row once the first
  //    pixel outside the triangle is found.
  void barycentricO2(ImageData& imageData, const Triangle& triangle);

private:
  // True projected depth from the (NDC) clip-space z when useProjectedDepth
  // is enabled. Falls back to the legacy inverse-w depth otherwise.
  double fragmentDepth(float ndcZ, double legacyDepth) const
  {
    return useProjectedDepth ? static_cast<double>(ndcZ)
                             : legacyDepth;
  }

  // Commits the winning fragment to the color/depth/fragment targets in one
  // place so color, position, normal and depth always update together.
  void commitFragment(ImageData& imageData,
                      int x, int y,
                      std::uint32_t pixel,
                      float ndcZ,
                      double legacyDepth,
                      const Vector3f& attribute,
                      const Vector3f& normal,
                      const float bary[3],
                      int triangleId);

    // Used by scanlineTriangle
  void scanline(ImageData& imageData,
                const Triangle& triangle,
                const Vertex4f& left,
                const Vertex4f& right,
                int y);

  // Used by all the barycentric drawing methods
  void barycentricPixel(ImageData& imageData,
                        int x, int y,
                        const Triangle& t,
                        float alp, float bet, float gam,
                        double w);

  // Used by barycentricO2
  int barycentricScanline(ImageData& imageData,
                          const Triangle& triangle,
                          int leftx, int rightx,
                          int y,
                          float area,
                          const float deltacols[3]);
}; // class TriangleDrawer
} // namespace al
} // namespace graphics

#endif