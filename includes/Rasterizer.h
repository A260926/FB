#ifndef RASTERIZER_H
#define RASTERIZER_H

#include <string>
#include "Renderer.h"
#include "ShadowMap.h"
#include "SectionPass.h"

// One triangle/pixel coverage produced by the shared raster traversal.
struct RasterSample {
  uint32_t pixel_index;
  Point2D pixel;
  double depth;          // camera axis depth
  Point3D world;         // world position of the fragment
  bool covered;          // top-left (section mode) / legacy coverage
  int sign;              // original triangle orientation, +1 or -1
};

class Rasterizer : public Renderer {
protected:
  std::vector<double> m_depth_buffer;
  std::vector<ShadowMap*> m_shadow_maps;
  SectionPass m_section;
public:
  Rasterizer();
  Rasterizer(const World* world);
  ~Rasterizer();

  inline const std::vector<double> get_depth_buffer() const { return m_depth_buffer; }

  virtual void render(const bool use_shade = true, const bool use_shadow_maps = true) = 0;
  virtual void export_output(const std::string output_path) const = 0;

  // World-space single plane sectioning. Image only: meshes, materials and
  // the World are never modified.
  void enableSection(const Vector3D& n, const double d, const RGBColor& color,
                     const RGBColor& diffuse, const RGBColor& specular);
  void updateSection(const Vector3D& n, const double d, const RGBColor& color,
                     const RGBColor& diffuse, const RGBColor& specular);
  void disableSection();
  bool sectionEnabled() const;
  // Normalized plane values of the active section configuration.
  Vector3D sectionPlaneNormal() const;
  double sectionPlaneD() const;

  // Readback of the latest section frame.
  const std::vector<unsigned char>& sectionCapMask() const;
  const std::vector<int32_t>& sectionCapObject() const;

public:
  // Shadows
  const void createShadowMaps();
  const double shadowFactor(const Point3D& pixel_world);
  // Interpolations
  const double calculateDepth(const Triangle3D& triangle_world, const Triangle2D& triangle_camera, const Point2D& pixel_camera) const;
  double rasterSampleDepth(const Triangle3D& triangle_world, const Triangle2D& triangle_raster, const Point2D& pixel_raster) const;
  //const Vector2D calculateTextureCoords(const Triangle3D& triangle_world, const Point3D& point_world) const;
  const Vector2D calculateTextureCoords(const Triangle3D& triangle_world, const Point3D& pixel_world, const Triangle2D& triangle_camera, const Point2D& pixel_camera) const;

  const RGBColor calculateColor(const Triangle3D& triangle_world, const Point3D& point_world) const;
  const Fragment calculateFragmentAttributes(const Triangle3D& triangle_world, const Point3D& point_world, const Triangle2D& triangle_screen, const Point2D& pixel_screen, const Material& material) const;

  // Transformations
  const Triangle2D rasterize(const Triangle3D& triangle_world) const;
  const Point2D rasterize(const Point3D& point) const;

  const Point3D unrasterize(const Point2D& point_raster, const double depth_from_camera) const;

  // Shared production traversal of one world-space triangle over the integer
  // raster pixels of its bounding box. The callback receives one
  // RasterSample per covered pixel (depth and world position are always
  // computed). With sectioning enabled the top-left winding-normalized
  // coverage rule is used and sign carries the original triangle orientation
  // for crossing counts; otherwise the legacy Triangle2D::contains rule is
  // used.
  template <class Fn>
  void traverseTriangle(const GeometryObject& object, const Triangle3D& triangle_world, Fn&& process) const {
    const uint16_t image_width = m_camera->get_width();
    const uint16_t image_height = m_camera->get_height();
    const Triangle2D triangle_raster = rasterize(triangle_world);
    const BoundingBox2D bbox_raster = triangle_raster.bbox();

    int orientation = 0;
    Point2D a, b, c;
    if (m_section.enabled()) {
      a = triangle_raster.v1;
      b = triangle_raster.v2;
      c = triangle_raster.v3;
      const double area2 = sectionEdgeE(a, b, c);
      if (area2 > 0.0) {
        orientation = 1;
      } else if (area2 < 0.0) {
        orientation = -1;
        std::swap(b, c);
      }
    }

    for (int32_t px = bbox_raster.min.x; px <= bbox_raster.max.x; ++px) {
      for (int32_t py = bbox_raster.min.y; py <= bbox_raster.max.y; ++py) {
        if (px < 0 || px >= image_width || py < 0 || py >= image_height) {
          continue;
        }
        const Point2D pixel(static_cast<double>(px), static_cast<double>(py));
        bool covered;
        if (m_section.enabled()) {
          if (orientation == 0) {
            continue; // Projected/degenerate face: no crossing contribution.
          }
          covered = topLeftContains(a, b, c, pixel);
        } else {
          covered = triangle_raster.contains(pixel);
        }
        if (!covered) {
          continue;
        }

        const double depth = rasterSampleDepth(triangle_world, triangle_raster, pixel);
        const Point3D pixel_world = unrasterize(pixel, depth);
        if (!m_camera->insideFrustrum(pixel, depth)) {
          continue;
        }

        RasterSample sample;
        sample.pixel_index = static_cast<uint32_t>(py) * image_width + static_cast<uint32_t>(px);
        sample.pixel = pixel;
        sample.depth = depth;
        sample.world = pixel_world;
        sample.covered = true;
        sample.sign = orientation;
        process(object, triangle_world, triangle_raster, sample);
      }
    }
  }

  // Top-left coverage on a triangle already normalized to E(a,b,c) > 0:
  // shared edges are included only by the edge whose dy > 0, or whose
  // dy == 0 and dx < 0. Every shared edge is therefore counted exactly once.
  static bool topLeftContains(const Point2D& a, const Point2D& b,
                              const Point2D& c, const Point2D& p) {
    const double e0 = sectionEdgeE(a, b, p);
    const double e1 = sectionEdgeE(b, c, p);
    const double e2 = sectionEdgeE(c, a, p);
    auto edgeIncludes = [](const Point2D& u, const Point2D& v, double e) {
      if (e > 0.0) return true;
      if (e < 0.0) return false;
      const double dx = v.x - u.x;
      const double dy = v.y - u.y;
      return dy > 0.0 || (dy == 0.0 && dx < 0.0);
    };
    return edgeIncludes(a, b, e0) && edgeIncludes(b, c, e1) && edgeIncludes(c, a, e2);
  }

  void exportDepthBuffer(const std::vector<double>& depth_buffer, const std::string output_path, const uint16_t image_width, const uint16_t image_height) const;

protected:
  // True when the section plane keeps this world point (n.p + d >= 0).
  bool sectionKeeps(const Point3D& point) const;
  // Cap normal: -normalize(n).
  Vector3D sectionCapNormal() const;
  // Build the cap fragment colors for one winning cap pixel.
  Fragment sectionCapFragment(const Point3D& point) const;
};

#endif /* RASTERIZER_H */
