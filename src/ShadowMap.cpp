#include "ShadowMap.h"
#include "OrthographicCamera.h"
#include "ForwardRasterizer.h"
#include <cmath>

ShadowMap::ShadowMap()
  : m_rasterizer(nullptr), m_camera(nullptr), m_light(nullptr) {
}

// Each directional light owns an orthographic camera looking down the light
// direction. The camera and its internal rasterizer are owned by this
// ShadowMap and released in the destructor.
ShadowMap::ShadowMap(const World * world, const Light* light) {
  m_light = light;
  m_rasterizer = new ForwardRasterizer(world);
  m_camera = new OrthographicCamera(m_light->getPosition(), Point3D(0, 0, 0), SHADOW_MAP_HEIGHT, SHADOW_MAP_WIDTH, m_rasterizer, 2, 1000000.0 * 2);
}

void ShadowMap::render() {
  // Render without shadow maps and shading: only depth is needed.
  m_rasterizer->render(false, false);

  // Depth buffer as seen from the light's point of view.
  m_shadow_map = m_rasterizer->get_depth_buffer();
}

const bool ShadowMap::texelOccluded(const int texel_x, const int texel_y, const double current_depth) const {
  const uint32_t index = (uint32_t)texel_y * SHADOW_MAP_WIDTH + texel_x;
  const double closest_depth = m_shadow_map[index];
  // Empty texels keep the clear value (far plane): no occluder was rendered.
  if (closest_depth >= m_camera->get_far_plane()) {
    return false;
  }
  // Bias stays in world units, measured along the light camera's view axis.
  return current_depth > closest_depth + SHADOW_MAP_BIAS;
}

const double ShadowMap::pointInShadow(const Point3D& point_world) const {
  // Depth along the axis of the orthographic camera that generated this map.
  const Point3D point_light_camera = m_camera->viewTransform(point_world);
  const double current_depth = point_light_camera.z;

  // Outside the light camera's near/far range: treated as unoccluded.
  if (current_depth < m_camera->get_near_plane() || current_depth > m_camera->get_far_plane()) {
    return 0.0;
  }

  const Point2D point_light_raster = m_rasterizer->rasterize(point_world);
  const int center_x = (int)std::round(point_light_raster.x);
  const int center_y = (int)std::round(point_light_raster.y);

#ifdef _PCF_SHADOWS
  // 3x3 neighborhood centered on the nearest texel. Taps falling outside the
  // shadow map count as unoccluded (occlusion 0).
  double occluded = 0.0;
  unsigned int taps = 0;
  for (int offset_y = -1; offset_y <= 1; ++offset_y) {
    for (int offset_x = -1; offset_x <= 1; ++offset_x) {
      const int texel_x = center_x + offset_x;
      const int texel_y = center_y + offset_y;
      if (texel_x >= 0 && texel_x < SHADOW_MAP_WIDTH && texel_y >= 0 && texel_y < SHADOW_MAP_HEIGHT) {
        occluded += texelOccluded(texel_x, texel_y, current_depth) ? 1.0 : 0.0;
      }
      ++taps;
    }
  }
  return occluded / (double)taps;
#else
  // Hard comparison using the nearest texel. Points projected outside the
  // shadow map are never occluded.
  if (center_x < 0 || center_x >= SHADOW_MAP_WIDTH || center_y < 0 || center_y >= SHADOW_MAP_HEIGHT) {
    return 0.0;
  }
  return texelOccluded(center_x, center_y, current_depth) ? 1.0 : 0.0;
#endif
}

ShadowMap::~ShadowMap() {
  delete m_rasterizer;
  delete m_camera;
}
