#ifndef RASTERIZER_H
#define RASTERIZER_H

#include <string>
#include "Renderer.h"
#include "ShadowMap.h"

class Rasterizer : public Renderer {
protected:
  std::vector<double> m_depth_buffer;
  // One shadow map per light, in the same order as the world's light list.
  std::vector<ShadowMap*> m_shadow_maps;

  // Deletes every shadow map, and with it the shadow camera/rasterizer that
  // each one owns. Called at the start of every render so that disabling
  // shadows, changing lights or rendering again never leaves stale maps.
  void clearShadowMaps();

  static int s_live_instances;

public:
  Rasterizer();
  Rasterizer(const World* world);
  virtual ~Rasterizer();

  inline const std::vector<double> get_depth_buffer() const { return m_depth_buffer; }

  virtual void render(const bool use_shade = true, const bool use_shadow_maps = true) = 0;
  virtual void export_output(const std::string output_path) const = 0;

  // Number of live Rasterizer instances (used by the lifetime regression to
  // verify that shadow resources are released).
  static int liveInstances() { return s_live_instances; }

public:
  // Shadows
  void createShadowMaps();
  // Occlusion ratio in [0, 1] for every light, in world light order. A light
  // is attenuated only by the factor of its own shadow map.
  const std::vector<double> shadowFactors(const Point3D& pixel_world) const;
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
