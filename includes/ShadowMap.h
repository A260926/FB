#pragma once

class Rasterizer;

#include <vector>
#include "Camera.h"
#include "Light.h"

class ShadowMap {
protected:
  // Render data
  Rasterizer* m_rasterizer;
  Camera * m_camera;
  // Data
  const Light * m_light;
  std::vector<double> m_shadow_map;
  // Constants
  const int SHADOW_MAP_WIDTH = 800;
  const int SHADOW_MAP_HEIGHT = 600;
  const double SHADOW_MAP_BIAS = 7.5;

public:
  ShadowMap();
  ShadowMap(const World * world, const Light* light);

  void render();
  // Returns the occlusion ratio in [0, 1] for point_world under this map's
  // light: 0 is fully lit, 1 is fully occluded.
  const double pointInShadow(const Point3D& point_world) const;

  // Hard nearest-texel comparison by default. Defining _PCF_SHADOWS at build
  // time enables the 3x3 percentage-closer filter by default; it can also be
  // toggled at runtime through this flag (used by the regression tests).
  static bool& pcfShadows();

  const Light* getLight() const { return m_light; }
  const Camera* getCamera() const { return m_camera; }
  const std::vector<double>& getShadowMap() const { return m_shadow_map; }

  ~ShadowMap();
};
