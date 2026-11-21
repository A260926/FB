#include "ForwardRasterizer.h"
#include <stdexcept>

ForwardRasterizer::ForwardRasterizer(){
}

ForwardRasterizer::ForwardRasterizer(const World* world)
  : Rasterizer(world) {

}

ForwardRasterizer::~ForwardRasterizer() {
}


void ForwardRasterizer::render(const bool use_shade, const bool use_shadow_maps) {
  // Sectioning is only defined for the shaded, shadowless path. Reject any
  // other parameter combination before the existing buffers are touched.
  if (sectionEnabled() && (!use_shade || use_shadow_maps)) {
    throw std::invalid_argument("section rendering requires render(true, false)");
  }

  const uint16_t image_width = m_camera->get_width();
  const uint16_t image_height = m_camera->get_height();
  const uint32_t pixel_count = image_height * image_width;

  m_pixels = std::vector<RGBColor>(pixel_count, BACKGROUND_COLOR);
  m_depth_buffer = std::vector<double>(pixel_count, m_camera->get_far_plane());

  if (use_shadow_maps) {
    this->createShadowMaps();
  }

  if (!sectionEnabled()) {
    for (auto& object : m_world->m_objects) {
      const std::vector<Triangle3D> triangles = object->triangles();
      for (const Triangle3D& triangle_world : triangles) {
        traverseTriangle(*object, triangle_world,
          [&](const GeometryObject& owner, const Triangle3D& tri,
              const Triangle2D& tri_raster, const RasterSample& sample) {
            const uint32_t i = sample.pixel_index;
            if (sample.depth < m_depth_buffer[i]) {
              const Fragment fragment = calculateFragmentAttributes(
                tri, sample.world, tri_raster, sample.pixel, *owner.material());
              m_depth_buffer[i] = sample.depth;
              if (use_shade) {
                const RGBColor object_color = Material::shade(m_world->m_lights, *m_world->m_camera, fragment);
                const RGBColor shadow = shadowFactor(sample.world);
                m_pixels[i] = object_color - shadow;
              }
            }
          });
      }
    }
    return;
  }

  /****************************** Section path ******************************/
  m_section.beginFrame(pixel_count);

  // Intersect every integer pixel ray with the section plane once.
  std::vector<Point3D> plane_hit(pixel_count);
  std::vector<double> plane_depth(pixel_count, m_camera->get_far_plane());
  std::vector<unsigned char> plane_valid(pixel_count, 0);
  for (int32_t py = 0; py < image_height; ++py) {
    for (int32_t px = 0; px < image_width; ++px) {
      const uint32_t i = py * image_width + px;
      Point3D hit;
      double depth;
      if (sectionRayPlane(m_section.plane(), *m_camera,
                          Point2D(static_cast<double>(px), static_cast<double>(py)),
                          hit, depth)) {
        plane_hit[i] = hit;
        plane_depth[i] = depth;
        plane_valid[i] = 1;
      }
    }
  }

  // 1. Retained regular surfaces: discard every fragment on the negative side
  // of the plane, keep the original attributes, material and interpolation.
  for (auto& object : m_world->m_objects) {
    const std::vector<Triangle3D> triangles = object->triangles();
    for (const Triangle3D& triangle_world : triangles) {
      traverseTriangle(*object, triangle_world,
        [&](const GeometryObject& owner, const Triangle3D& tri,
            const Triangle2D& tri_raster, const RasterSample& sample) {
          if (!sectionKeeps(sample.world)) {
            return;
          }
          const uint32_t i = sample.pixel_index;
          if (sample.depth < m_depth_buffer[i]) {
            const Fragment fragment = calculateFragmentAttributes(
              tri, sample.world, tri_raster, sample.pixel, *owner.material());
            m_depth_buffer[i] = sample.depth;
            m_section.setSurface(i);
            const RGBColor object_color = Material::shade(m_world->m_lights, *m_world->m_camera, fragment);
            m_pixels[i] = object_color;
          }
        });
    }
  }

  // 2. Solid test per object (nonzero integer winding) and cap competition in
  // the same axis depth buffer. Objects are judged independently.
  std::vector<int32_t> winding(pixel_count, 0);
  for (int32_t object_index = 0;
       object_index < static_cast<int32_t>(m_world->m_objects.size());
       ++object_index) {
    GeometryObject* object = m_world->m_objects[object_index];
    std::fill(winding.begin(), winding.end(), 0);

    const std::vector<Triangle3D> triangles = object->triangles();
    for (const Triangle3D& triangle_world : triangles) {
      traverseTriangle(*object, triangle_world,
        [&](const GeometryObject&, const Triangle3D&,
            const Triangle2D&, const RasterSample& sample) {
          const uint32_t i = sample.pixel_index;
          if (plane_valid[i] && sample.depth >= plane_depth[i]) {
            // Directed crossing from the section point toward the far plane.
            winding[i] += sample.sign;
          }
        });
    }

    for (uint32_t i = 0; i < pixel_count; ++i) {
      if (!plane_valid[i] || winding[i] == 0) {
        continue; // Outside this object or a cavity: never fill it.
      }
      // Strict comparison keeps an equal-depth regular surface and an earlier
      // object's cap.
      if (plane_depth[i] < m_depth_buffer[i]) {
        const Fragment fragment = sectionCapFragment(plane_hit[i]);
        m_depth_buffer[i] = plane_depth[i];
        m_section.setCap(i, object_index);
        m_pixels[i] = Material::shade(m_world->m_lights, *m_world->m_camera, fragment);
      }
    }
  }
}

void ForwardRasterizer::export_output(const std::string output_path) const {
  const uint16_t image_width = m_camera->get_width();
  const uint16_t image_height = m_camera->get_height();

  exportDepthBuffer(m_depth_buffer, "f_d_"+output_path, image_width, image_height);
  exportImage(m_pixels, "f_" + output_path, image_width, image_height);
}
