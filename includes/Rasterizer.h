#ifndef RASTERIZER_H
#define RASTERIZER_H

#include <algorithm>
#include <cstdint>
#include <string>
#include "Renderer.h"
#include "ShadowMap.h"
#include "SectionPass.h"

class Rasterizer : public Renderer {
protected:
  std::vector<double> m_depth_buffer;
  std::vector<ShadowMap*> m_shadow_maps;

  // World-space single plane sectioning state
  bool m_section_enabled = false;
  SectionConfig m_section_config;
  // Per-pixel data of the caps that won the depth test in the latest frame
  std::vector<SectionCap> m_section_caps;
  std::vector<uint8_t> m_section_cap_mask;
  std::vector<int32_t> m_section_cap_object;

public:
  Rasterizer();
  Rasterizer(const World* world);
  ~Rasterizer();

  inline const std::vector<double> get_depth_buffer() const { return m_depth_buffer; }

  virtual void render(const bool use_shade = true, const bool use_shadow_maps = true) = 0;
  virtual void export_output(const std::string output_path) const = 0;

  // World-space single plane sectioning. The plane keeps the half-space
  // n . p + d >= 0 and the normal is normalized together with d.
  void enableSection(const Vector3D& n, const double d, const RGBColor& color,
                     const RGBColor& diffuse, const RGBColor& specular);
  void updateSection(const Vector3D& n, const double d, const RGBColor& color,
                     const RGBColor& diffuse, const RGBColor& specular);
  void disableSection();
  bool sectionEnabled() const { return m_section_enabled; }
  const SectionConfig& sectionConfig() const { return m_section_config; }

  // Read-back of the caps that were visible in the latest sectioned frame.
  // The object index is SECTION_INVALID_OBJECT for pixels without a cap.
  // Empty after a non-sectioned render or after disableSection().
  const std::vector<uint8_t>& sectionCapMask() const { return m_section_cap_mask; }
  const std::vector<int32_t>& sectionCapObject() const { return m_section_cap_object; }
  const std::vector<SectionCap>& sectionCaps() const { return m_section_caps; }

protected:
  // Resets the per-frame section read-back buffers (does not touch geometry,
  // materials or the world).
  void clearSectionFrameData();
  // Refuses render parameter combinations that are unsupported while
  // sectioning is enabled, before any buffer is modified.
  void validateSectionRenderArgs(const bool use_shade,
                                 const bool use_shadow_maps) const;

  // Shared production traversal entry used by both render paths and by the
  // section winding pass. Calls visitor(index, depth, world_position,
  // prepared, triangle) once for every integer pixel covered by the triangle.
  template <class Visitor>
  void traverseTriangle(const Triangle3D& triangle_world,
                        const Triangle2D& triangle_raster,
                        const uint32_t image_width,
                        const uint32_t image_height,
                        Visitor&& visitor) const;

  // Builds the section caps for one frame: for every pixel, the camera ray is
  // intersected with the plane and the non-zero integer winding number of
  // every object is accumulated with the shared triangle traversal. Caps that
  // lose the depth test against already drawn ordinary surfaces are dropped.
  void buildSectionCaps();

public:
  // Shadows
  const void createShadowMaps();
  const double shadowFactor(const Point3D& pixel_world);
  // Interpolations
  const double calculateDepth(const Triangle3D& triangle_world, const Triangle2D& triangle_camera, const Point2D& pixel_camera) const;
  //const Vector2D calculateTextureCoords(const Triangle3D& triangle_world, const Point3D& point_world) const;
  const Vector2D calculateTextureCoords(const Triangle3D& triangle_world, const Point3D& pixel_world, const Triangle2D& triangle_camera, const Point2D& pixel_camera) const;

  const RGBColor calculateColor(const Triangle3D& triangle_world, const Point3D& point_world) const;
  const Fragment calculateFragmentAttributes(const Triangle3D& triangle_world, const Point3D& point_world, const Triangle2D& triangle_screen, const Point2D& pixel_screen, const Material& material) const;

  // Transformations
  const Triangle2D rasterize(const Triangle3D& triangle_world) const;
  const Point2D rasterize(const Point3D& point) const;

  const Point3D unrasterize(const Point2D& point_raster, const double depth_from_camera) const;

  // Depth used by the shared traversal: inverse axis-depth reconstruction
  // under perspective (and whenever sectioning is disabled, preserving the
  // old behavior); linear axis-depth interpolation under orthographic
  // projection while sectioning is enabled.
  const double fragmentDepth(const Triangle3D& triangle_world,
                             const Triangle2D& triangle_raster,
                             const Point2D& pixel_raster) const;


  void exportDepthBuffer(const std::vector<double>& depth_buffer, const std::string output_path, const uint16_t image_width, const uint16_t image_height) const;
};

template <class Visitor>
void Rasterizer::traverseTriangle(const Triangle3D& triangle_world,
                                  const Triangle2D& triangle_raster,
                                  const uint32_t image_width,
                                  const uint32_t image_height,
                                  Visitor&& visitor) const {
  const BoundingBox2D bbox_raster = triangle_raster.bbox();
  const int32_t min_x = std::max<int32_t>(0, bbox_raster.min.x);
  const int32_t max_x =
    std::min<int32_t>(static_cast<int32_t>(image_width) - 1, bbox_raster.max.x);
  const int32_t min_y = std::max<int32_t>(0, bbox_raster.min.y);
  const int32_t max_y =
    std::min<int32_t>(static_cast<int32_t>(image_height) - 1, bbox_raster.max.y);
  if (min_x > max_x || min_y > max_y) {
    return;
  }

  if (m_section_enabled) {
    // Shared top-left rule traversal with explicit crossing orientation.
    const PreparedTriangle prepared =
      section_pass::prepareTriangle(triangle_raster);
    if (prepared.degenerate) {
      return;
    }
    for (int32_t pixel_x = min_x; pixel_x <= max_x; ++pixel_x) {
      for (int32_t pixel_y = min_y; pixel_y <= max_y; ++pixel_y) {
        const Point2D pixel_raster(static_cast<double>(pixel_x),
                                   static_cast<double>(pixel_y));
        if (!section_pass::triangleContains(prepared, pixel_raster)) {
          continue;
        }
        const double depth =
          fragmentDepth(triangle_world, triangle_raster, pixel_raster);
        const Point3D pixel_world = unrasterize(pixel_raster, depth);
        if (!m_camera->insideFrustrum(pixel_raster, depth)) {
          continue;
        }
        const uint32_t index =
          static_cast<uint32_t>(pixel_y) * image_width + pixel_x;
        visitor(index, depth, pixel_world, prepared, triangle_world);
      }
    }
    return;
  }

  // Sectioning disabled: the original coverage rule is preserved exactly.
  for (int32_t pixel_x = min_x; pixel_x <= max_x; ++pixel_x) {
    for (int32_t pixel_y = min_y; pixel_y <= max_y; ++pixel_y) {
      const Point2D pixel_raster(static_cast<double>(pixel_x),
                                 static_cast<double>(pixel_y));
      if (!triangle_raster.contains(pixel_raster)) {
        continue;
      }
      const double depth =
        fragmentDepth(triangle_world, triangle_raster, pixel_raster);
      const Point3D pixel_world = unrasterize(pixel_raster, depth);
      if (!m_camera->insideFrustrum(pixel_raster, depth)) {
        continue;
      }
      const uint32_t index =
        static_cast<uint32_t>(pixel_y) * image_width + pixel_x;
      const PreparedTriangle unused;
      visitor(index, depth, pixel_world, unused, triangle_world);
    }
  }
}

#endif /* RASTERIZER_H */
