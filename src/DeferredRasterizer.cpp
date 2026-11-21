#include "DeferredRasterizer.h"
#include <algorithm>
#include <stdexcept>

DeferredRasterizer::DeferredRasterizer() {
}

DeferredRasterizer::DeferredRasterizer(const World* world)
  : Rasterizer(world) {

}

DeferredRasterizer::~DeferredRasterizer() {
}


void DeferredRasterizer::render(const bool use_shade, const bool use_shadow_maps) {
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
  m_normal_buffer = std::vector<RGBColor>(pixel_count, BACKGROUND_COLOR);
  m_color_buffer = std::vector<RGBColor>(pixel_count, BACKGROUND_COLOR);
  m_diffuse_buffer = std::vector<RGBColor>(pixel_count, BACKGROUND_COLOR);
  m_specular_buffer = std::vector<RGBColor>(pixel_count, BACKGROUND_COLOR);

  if (use_shadow_maps) {
    this->createShadowMaps();
  }

  auto writeGBuffer = [&](uint32_t i, const Fragment& fragment) {
    m_normal_buffer[i] = (RGBColor)fragment.normal;
    m_color_buffer[i] = fragment.color;
    m_diffuse_buffer[i] = fragment.diffuse;
    m_specular_buffer[i] = fragment.specular;
  };

  if (!sectionEnabled()) {
    // Render color, depth and normals through the shared traversal.
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
              writeGBuffer(i, fragment);
            }
          });
      }
    }

    shadeGBuffer(use_shadow_maps);
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

  // 1. Retained regular surfaces keep their original attributes and material
  // and are written straight into the shared GBuffer.
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
            writeGBuffer(i, fragment);
          }
        });
    }
  }

  // 2. Solid test per object (nonzero integer winding). Winning cap fragments
  // are written into the same GBuffer before any lighting happens.
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
            winding[i] += sample.sign;
          }
        });
    }

    for (uint32_t i = 0; i < pixel_count; ++i) {
      if (!plane_valid[i] || winding[i] == 0) {
        continue;
      }
      if (plane_depth[i] < m_depth_buffer[i]) {
        const Fragment fragment = sectionCapFragment(plane_hit[i]);
        m_depth_buffer[i] = plane_depth[i];
        m_section.setCap(i, object_index);
        writeGBuffer(i, fragment);
      }
    }
  }

  // Lighting runs once over surfaces and caps alike.
  shadeGBuffer(false);
}

void DeferredRasterizer::shadeGBuffer(const bool use_shadow_maps) {
  const uint16_t image_width = m_camera->get_width();
  const uint16_t image_height = m_camera->get_height();

  for (int x = 0; x < image_width; ++x) {
    for (int y = 0; y < image_height; ++y) {
      const uint32_t i = y * image_width + x;
      const double depth = m_depth_buffer[i];
      if (depth < m_camera->get_far_plane()) {
        const Point2D pixel_raster(x, y);
        const Point3D pixel_world = unrasterize(pixel_raster, m_depth_buffer[i]);

        Fragment fragment{
          pixel_world,
          m_color_buffer[i],
          m_diffuse_buffer[i],
          m_specular_buffer[i],
          (Vector3D)m_normal_buffer[i]
        };
        const RGBColor object_color = Material::shade(m_world->m_lights, *m_world->m_camera, fragment);
        RGBColor pixel = object_color;
        if (use_shadow_maps) {
          const RGBColor shadow_factor = shadowFactor(fragment.position);
          pixel = object_color - shadow_factor;
        }
        m_pixels[i] = pixel;
      }
      else {
        m_pixels[i] = BACKGROUND_COLOR;
      }
    }
  }
}

void DeferredRasterizer::export_output(const std::string output_path) const {
  const uint16_t image_width = m_camera->get_width();
  const uint16_t image_height = m_camera->get_height();

  exportDepthBuffer(m_depth_buffer, "d_depth.bmp", image_width, image_height);
  exportImage(m_normal_buffer, "d_normals.bmp", image_width, image_height);
  exportImage(m_color_buffer, "d_colors.bmp", image_width, image_height);
  exportImage(m_diffuse_buffer, "d_diffuse.bmp", image_width, image_height);
  exportImage(m_specular_buffer, "d_specular.bmp", image_width, image_height);
  exportImage(m_pixels, "d_" + output_path, image_width, image_height);
}
