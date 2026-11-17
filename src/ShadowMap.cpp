#include "ShadowMap.h"
#include "OrthographicCamera.h"
#include "ForwardRasterizer.h"

#include <cmath>

namespace {
// Default comes from the build configuration; tests (and callers that want
// hard shadows at runtime) can flip this switch.
bool g_pcf_shadows =
#ifdef _PCF_SHADOWS
  true;
#else
  false;
#endif

bool sampleInShadow(const std::vector<double>& shadow_map,
                    const int width, const int height,
                    const int x, const int y,
                    const double current_depth,
                    const double bias) {
  // Taps outside the map are considered unoccluded.
  if (x < 0 || x >= width || y < 0 || y >= height) {
    return false;
  }
  const double closest_depth = shadow_map[(size_t)y * width + x];
  return current_depth > closest_depth + bias;
}
} // namespace

ShadowMap::ShadowMap() {
}

ShadowMap::ShadowMap(const World * world, const Light * light) {
  m_light = light;
  m_rasterizer = new ForwardRasterizer(world);
  m_camera = new OrthographicCamera(m_light->getPosition(), Point3D(0, 0, 0), SHADOW_MAP_HEIGHT, SHADOW_MAP_WIDTH, m_rasterizer, 2, 1000000.0 * 2);
}

void ShadowMap::render() {
  // Render without shadow maps and shading: only depth
  m_rasterizer->render(false, false);

  // Shadow map from light's POV. Under the orthographic shadow camera the
  // depth buffer holds the axial (view-space z) depth.
  m_shadow_map = m_rasterizer->get_depth_buffer();
  m_rasterizer->exportDepthBuffer(m_shadow_map, "shadow_map.bmp", SHADOW_MAP_WIDTH, SHADOW_MAP_HEIGHT);
}

bool& ShadowMap::pcfShadows() {
  return g_pcf_shadows;
}

const double ShadowMap::pointInShadow(const Point3D& point_world) const {
  if (m_shadow_map.empty()) {
    return 0.0;
  }

  // Depth of the receiving point along the shadow camera's axis. Comparing in
  // the orthographic camera's own depth space keeps the world-unit bias
  // meaningful.
  const double current_depth = m_camera->viewTransform(point_world).z;
  const double bias = SHADOW_MAP_BIAS;

  // Outside the shadow camera's near/far range: no occlusion information.
  if (current_depth < m_camera->get_near_plane() || current_depth > m_camera->get_far_plane()) {
    return 0.0;
  }

  const Point2D point_light_space = m_rasterizer->rasterize(point_world);

  // Outside the shadow map image: considered unoccluded.
  if (point_light_space.x < 0 || point_light_space.x >= SHADOW_MAP_WIDTH ||
      point_light_space.y < 0 || point_light_space.y >= SHADOW_MAP_HEIGHT) {
    return 0.0;
  }

  const int center_x = (int)std::floor(point_light_space.x + 0.5);
  const int center_y = (int)std::floor(point_light_space.y + 0.5);

  if (!g_pcf_shadows) {
    // Default hard comparison against the nearest texel.
    return sampleInShadow(m_shadow_map, SHADOW_MAP_WIDTH, SHADOW_MAP_HEIGHT,
                          center_x, center_y, current_depth, bias) ? 1.0 : 0.0;
  }

  // 3x3 percentage-closer filter centred on the nearest texel. Taps outside
  // the map count as unoccluded.
  int occluded = 0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (sampleInShadow(m_shadow_map, SHADOW_MAP_WIDTH, SHADOW_MAP_HEIGHT,
                         center_x + dx, center_y + dy, current_depth, bias)) {
        ++occluded;
      }
    }
  }
  return occluded / 9.0;
}

ShadowMap::~ShadowMap() {
  delete m_rasterizer;
  delete m_camera;
}
