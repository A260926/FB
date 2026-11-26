#pragma once

class Rasterizer;

#include <vector>
#include "Camera.h"
#include "Light.h"

class ShadowMap {
protected:
  // Render data. Both are owned by the ShadowMap and destroyed with it.
  Rasterizer* m_rasterizer;
  Camera * m_camera;
  // Data
  const Light * m_light;
  std::vector<double> m_shadow_map;
  // Constants
  const int SHADOW_MAP_WIDTH = 800;
  const int SHADOW_MAP_HEIGHT = 600;
  // World-space depth bias applied along the light camera's view axis.
  const double SHADOW_MAP_BIAS = 7.5;
public:
  ShadowMap();
  ShadowMap(const World * world, const Light* light);

  void render();
  // Returns the occlusion ratio of the point for this light, in [0, 1]:
  // 0 means fully lit, 1 means fully occluded. Points outside the shadow map
  // or outside the light camera's near/far range are never occluded.
  // Hard shadows use the nearest texel; with _PCF_SHADOWS the 3x3 neighborhood
  // around that texel is averaged (out-of-map taps count as unoccluded).
  const double pointInShadow(const Point3D& point_world) const;

  const Camera* camera() const { return m_camera; }
  const Rasterizer* rasterizer() const { return m_rasterizer; }
  int width() const { return SHADOW_MAP_WIDTH; }
  int height() const { return SHADOW_MAP_HEIGHT; }
  const std::vector<double>& depthBuffer() const { return m_shadow_map; }

  ~ShadowMap();

private:
  // Single hard comparison against one shadow-map texel.
  const bool texelOccluded(const int texel_x, const int texel_y, const double current_depth) const;
};
