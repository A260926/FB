#ifndef RASTERIZER_H
#define RASTERIZER_H

#include <string>
#include "Renderer.h"
#include "ShadowMap.h"

class Rasterizer : public Renderer {
protected:
  std::vector<double> m_depth_buffer;
  std::vector<ShadowMap*> m_shadow_maps;
public:
  Rasterizer();
  Rasterizer(const World* world);
  ~Rasterizer();
  
  inline const std::vector<double> get_depth_buffer() const { return m_depth_buffer; }

  virtual void render(const bool use_shade = true, const bool use_shadow_maps = true) = 0;
  virtual void export_output(const std::string output_path) const = 0;

  // Renders a single thin-lens pupil frame.
  //
  // The renderer's camera (m_camera) must already be the pupil camera set by
  // the ThinLens entry. Geometry coverage, perspective-correct attributes,
  // unrasterization and the independent depth test all run through that camera,
  // and the material observation direction is evaluated with pupil_camera as
  // well, so projection and shading never disagree and the center world camera
  // is never read. Shadows are disabled. The frame is written into the caller's
  // buffers; background pixels keep BACKGROUND_COLOR and therefore participate
  // in the outer weighted average.
  virtual void renderPupilFrame(const Camera& pupil_camera,
                                std::vector<RGBColor>& out_color,
                                std::vector<double>& out_depth) = 0;

  // Separate center pinhole geometry draw. Returns an independently computed
  // depth buffer (background entries hold the far plane); it is never the
  // averaged or last-sample pupil depth.
  std::vector<double> renderCenterDepth(const Camera& center_camera);

  // Writes an externally accumulated frame through the original BMP exporter.
  void exportPupilImage(const std::vector<RGBColor>& pixels, const std::string output_path) const;

  // Writes a frame whose dimensions are supplied explicitly (e.g. a side-by-side
  // comparison composite), still through the original BMP exporter.
  void exportImageSized(const std::vector<RGBColor>& pixels, const std::string output_path,
                        uint16_t image_width, uint16_t image_height) const;

protected:
  // Per-pixel geometry result shared by the forward and deferred pupil paths so
  // coverage, perspective interpolation and the independent depth test are run
  // exactly once with the same pupil camera. hit[i] is false for background.
  struct PupilGbuffer {
    std::vector<double> depth;
    std::vector<Fragment> fragment;
    std::vector<char> hit;
  };
  void rasterizePupilGeometry(const Camera& pupil_camera, PupilGbuffer& gbuffer) const;

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


  void exportDepthBuffer(const std::vector<double>& depth_buffer, const std::string output_path, const uint16_t image_width, const uint16_t image_height) const;
};

#endif /* RASTERIZER_H */

