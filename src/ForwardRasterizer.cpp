#include "ForwardRasterizer.h"

ForwardRasterizer::ForwardRasterizer(){
}

ForwardRasterizer::ForwardRasterizer(const World* world)
  : Rasterizer(world) {

}

ForwardRasterizer::~ForwardRasterizer() {
}


void ForwardRasterizer::render(const bool use_shade, const bool use_shadow_maps) {
  // Reject unsupported sectioning parameter combinations before touching any
  // buffer, so the previous configuration and image stay intact on failure.
  this->validateSectionRenderArgs(use_shade, use_shadow_maps);

  const uint16_t image_width = m_camera->get_width();
  const uint16_t image_height = m_camera->get_height();

  m_pixels = std::vector<RGBColor>(image_height * image_width, BACKGROUND_COLOR);
  m_depth_buffer = std::vector<double>(image_height * image_width, m_camera->get_far_plane());
  this->clearSectionFrameData();

  if (use_shadow_maps) {
    this->createShadowMaps();
  }

  for (auto& object : m_world->m_objects) {
    const std::vector<Triangle3D> triangles = object->triangles();
    for (auto& triangle_world : triangles) {
      const Triangle2D triangle_raster = this->rasterize(triangle_world);
      this->traverseTriangle(triangle_world, triangle_raster, image_width,
        image_height,
        [&](const uint32_t i, const double depth, const Point3D& pixel_world,
            const PreparedTriangle& /*prepared*/,
            const Triangle3D& triangle) {
          // Ordinary surfaces keep only the retained half-space n.p + d >= 0.
          if (m_section_enabled &&
              m_section_config.n * Vector3D(pixel_world) + m_section_config.d < 0.0) {
            return;
          }
          if (depth < m_depth_buffer[i]) {
            m_depth_buffer[i] = depth;
            if (use_shade) {
              const Fragment fragment = calculateFragmentAttributes(triangle, pixel_world, triangle_raster, Point2D(i % image_width, i / image_width), *object->material());
              const RGBColor object_color = Material::shade(m_world->m_lights, *m_world->m_camera, fragment);
              const RGBColor shadow = shadowFactor(pixel_world);
              m_pixels[i] = object_color - shadow;
            }
          }
        });
    }
  }

  if (m_section_enabled) {
    // Caps compete in the same axis-depth buffer as the retained surfaces.
    this->buildSectionCaps();

    // Shade only the caps that actually won; shadow maps are not extended.
    for (uint32_t i = 0; i < m_section_caps.size(); ++i) {
      if (!m_section_cap_mask[i]) {
        continue;
      }
      const SectionCap& cap = m_section_caps[i];
      const Fragment fragment(cap.position, cap.color, cap.diffuse,
                              cap.specular, cap.normal);
      m_pixels[i] = Material::shade(m_world->m_lights, *m_world->m_camera,
                                    fragment);
    }
  }
}

void ForwardRasterizer::export_output(const std::string output_path) const {
  const uint16_t image_width = m_camera->get_width();
  const uint16_t image_height = m_camera->get_height();

  exportDepthBuffer(m_depth_buffer, "f_d_"+output_path, image_width, image_height);
  exportImage(m_pixels, "f_" + output_path, image_width, image_height);
}
