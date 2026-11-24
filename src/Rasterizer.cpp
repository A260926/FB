#include "Rasterizer.h"
#include "OrthographicCamera.h"
#include "ForwardRasterizer.h"
#include <stdexcept>

Rasterizer::Rasterizer()
  : Renderer() {

}

Rasterizer::Rasterizer(const World* world)
  : Renderer(world) {

}

Rasterizer::~Rasterizer() {
}

void Rasterizer::enableSection(const Vector3D& n, const double d,
                               const RGBColor& color, const RGBColor& diffuse,
                               const RGBColor& specular) {
  // Validation happens first: on failure the previous configuration and the
  // existing images are left untouched.
  m_section_config =
    section_pass::validateConfig(n, d, color, diffuse, specular);
  m_section_enabled = true;
  clearSectionFrameData();
}

void Rasterizer::updateSection(const Vector3D& n, const double d,
                               const RGBColor& color, const RGBColor& diffuse,
                               const RGBColor& specular) {
  const SectionConfig next =
    section_pass::validateConfig(n, d, color, diffuse, specular);
  m_section_config = next;
  m_section_enabled = true;
  clearSectionFrameData();
}

void Rasterizer::disableSection() {
  m_section_enabled = false;
  clearSectionFrameData();
}

void Rasterizer::clearSectionFrameData() {
  m_section_caps.clear();
  m_section_cap_mask.clear();
  m_section_cap_object.clear();
}

void Rasterizer::validateSectionRenderArgs(const bool use_shade,
                                           const bool use_shadow_maps) const {
  if (m_section_enabled && (!use_shade || use_shadow_maps)) {
    throw std::invalid_argument(
      "Sectioning requires render(true, false): shading must be enabled and "
      "shadow maps are not extended to section caps");
  }
}

const double Rasterizer::fragmentDepth(const Triangle3D& triangle_world,
                                       const Triangle2D& triangle_raster,
                                       const Point2D& pixel_raster) const {
  if (m_section_enabled &&
      dynamic_cast<const OrthographicCamera*>(m_camera) != nullptr) {
    // Orthographic projection: axis depth is affine in screen space, so it
    // is interpolated linearly from the camera-space vertex depths.
    const double z1 = m_camera->viewTransform(triangle_world.v1.position).z;
    const double z2 = m_camera->viewTransform(triangle_world.v2.position).z;
    const double z3 = m_camera->viewTransform(triangle_world.v3.position).z;
    double u, v, w;
    triangle_raster.calculateBarycentricCoords(u, v, w, pixel_raster);
    return z1 * u + z2 * v + z3 * w;
  }
  // Perspective: depth is reconstructed from inverse axis depths.
  return calculateDepth(triangle_world, triangle_raster, pixel_raster);
}

// For every pixel whose camera ray meets the section plane inside the
// frustum, accumulate the directed crossings of each object's triangles
// lying between the section point and the far plane. A non-zero integer
// winding number means the section point is solid for that object. Objects
// are evaluated independently and caps compete in the same axis-depth
// buffer as the ordinary surfaces.
void Rasterizer::buildSectionCaps() {
  const uint16_t image_width = m_camera->get_width();
  const uint16_t image_height = m_camera->get_height();
  const uint32_t pixel_count =
    static_cast<uint32_t>(image_width) * image_height;

  clearSectionFrameData();
  m_section_caps.resize(pixel_count);
  m_section_cap_mask.assign(pixel_count, 0);
  m_section_cap_object.assign(pixel_count, SECTION_INVALID_OBJECT);

  // Camera ray / section plane intersection for every integer pixel.
  std::vector<Point3D> hit_positions(pixel_count);
  std::vector<double> hit_depths(pixel_count, 0.0);
  std::vector<char> hit_valid(pixel_count, 0);
  for (int32_t pixel_y = 0; pixel_y < image_height; ++pixel_y) {
    for (int32_t pixel_x = 0; pixel_x < image_width; ++pixel_x) {
      const uint32_t index =
        static_cast<uint32_t>(pixel_y) * image_width + pixel_x;
      const Point2D pixel_raster(static_cast<double>(pixel_x),
                                 static_cast<double>(pixel_y));
      Point3D hit_world;
      double hit_depth = 0.0;
      if (section_pass::rayPlaneHit(*m_camera, pixel_raster, m_section_config,
                                    hit_world, hit_depth)) {
        hit_positions[index] = hit_world;
        hit_depths[index] = hit_depth;
        hit_valid[index] = 1;
      }
    }
  }

  std::vector<int32_t> winding(pixel_count, 0);
  Vector3D cap_normal = m_section_config.n * -1.0;
  cap_normal.normalize();

  for (uint32_t object_index = 0;
       object_index < m_world->m_objects.size(); ++object_index) {
    GeometryObject* object = m_world->m_objects[object_index];
    std::fill(winding.begin(), winding.end(), 0);

    const std::vector<Triangle3D> triangles = object->triangles();
    for (const Triangle3D& triangle_world : triangles) {
      const Triangle2D triangle_raster = rasterize(triangle_world);
      traverseTriangle(triangle_world, triangle_raster, image_width,
                       image_height,
                       [&](const uint32_t index, const double depth,
                           const Point3D& /*pixel_world*/,
                           const PreparedTriangle& prepared,
                           const Triangle3D& /*triangle*/) {
        if (!hit_valid[index] || depth <= hit_depths[index]) {
          return;
        }
        // Projected-degenerate triangles never reach this visitor.
        winding[index] += prepared.front ? 1 : -1;
      });
    }

    // Non-zero winding (never parity): the section point lies inside this
    // object. Earlier objects win equal-depth caps because the depth test is
    // strict and objects are visited in world order.
    for (uint32_t index = 0; index < pixel_count; ++index) {
      if (!hit_valid[index] || winding[index] == 0) {
        continue;
      }
      if (hit_depths[index] < m_depth_buffer[index]) {
        m_depth_buffer[index] = hit_depths[index];
        SectionCap cap;
        cap.position = hit_positions[index];
        cap.normal = cap_normal;
        cap.depth = hit_depths[index];
        cap.object_index = object_index;
        cap.color = m_section_config.color;
        cap.diffuse = m_section_config.diffuse;
        cap.specular = m_section_config.specular;
        m_section_caps[index] = cap;
        m_section_cap_mask[index] = 1;
        m_section_cap_object[index] =
          static_cast<int32_t>(object_index);
      }
    }
  }
}


const Fragment Rasterizer::calculateFragmentAttributes(const Triangle3D& triangle_world, const Point3D& point_world, const Triangle2D& triangle_screen, const Point2D& pixel_screen, const Material& material) const {
  const Vector2D text_coords = calculateTextureCoords(triangle_world, point_world, triangle_screen, pixel_screen);
  const Fragment f {
    point_world,
    calculateColor(triangle_world, point_world),
    material.getDiffuseColor(text_coords), 
    material.getSpecularColor(text_coords),
    material.getNormal(triangle_world, text_coords)
  };
  
  return f;
}

// Calculate depth in screen space and then interpolate using inverse function (1/z) 
const double Rasterizer::calculateDepth(const Triangle3D& triangle_world, const Triangle2D& triangle_raster, const Point2D& pixel_raster) const {
  const Triangle3D triangle_camera = Triangle3D(
    Vertex3D (m_camera->viewTransform(triangle_world.v1.position), triangle_world.v1.color, triangle_world.v1.texture_coords, triangle_world.v1.normal),
    Vertex3D(m_camera->viewTransform(triangle_world.v2.position), triangle_world.v2.color, triangle_world.v2.texture_coords, triangle_world.v1.normal),
    Vertex3D(m_camera->viewTransform(triangle_world.v3.position), triangle_world.v3.color, triangle_world.v3.texture_coords, triangle_world.v1.normal)
  );

  // Calculate barycentric coords in camera space
  double u, v, w;
  triangle_raster.calculateBarycentricCoords(u, v, w, pixel_raster);
  // Interpolate Z in 3D using inverse function and barycentric coordinates in 2D
  const double depth = 1 / 
    (
      (1 / triangle_camera.v1.position.z) * u + 
      (1 / triangle_camera.v2.position.z) * v + 
      (1 / triangle_camera.v3.position.z) * w
    );
  return depth;
}

const Vector2D Rasterizer::calculateTextureCoords(const Triangle3D& triangle_world, const Point3D& pixel_world, const Triangle2D& triangle_camera, const Point2D& pixel_camera) const {
  // Calculate barycentric coords in screen space  (inverse linear)
  double u, v, w;
#ifdef _AFFINE_TEXTURES
  triangle_camera.calculateBarycentricCoords(u, v, w, pixel_camera);
#endif 
#ifdef _PERSPECTIVE_TEXTURES
  triangle_world.calculateBarycentricCoords(u, v, w, pixel_world);
#endif 

  const Vector2D texture_coords = 
    triangle_world.v1.texture_coords * u + 
    triangle_world.v2.texture_coords * v +
    triangle_world.v3.texture_coords * w;

  const Vector2D texture_coords_abs_screen {
    abs(texture_coords.x),
    abs(texture_coords.y)
  };

  return texture_coords_abs_screen;

  /*

  // Divide texture attribute at vertices by their z-coord
  Vector2D v1t = triangle_world.v1.texture_coords / abs(triangle_world.v1.position.z);
  Vector2D v2t = triangle_world.v2.texture_coords / abs(triangle_world.v2.position.z);
  Vector2D v3t = triangle_world.v3.texture_coords / abs(triangle_world.v3.position.z);

  float v1t_ = 1.0 / abs(triangle_world.v1.position.z);
  float v2t_ = 1.0 / abs(triangle_world.v2.position.z);
  float v3t_ = 1.0 / abs(triangle_world.v3.position.z);

  const Vector2D texture_coords2 =
    v1t * u +
    v2t * v +
    v3t * w;

  const float z = 1.0 / 
    (
      v1t_ * u +
      v2t_ * v +
      v3t_ * w
    );

  const Vector2D result = {
    texture_coords2.x * z,
    texture_coords2.y * z
  };

  return result;
  */
}

const RGBColor Rasterizer::calculateColor(const Triangle3D& triangle_world, const Point3D& point_world) const {
  // Calculate barycentric coords in camera space
  double u, v, w;
  triangle_world.calculateBarycentricCoords(u, v, w, point_world);
  const RGBColor new_color = triangle_world.v1.color * u + triangle_world.v2.color * v + triangle_world.v3.color * w;
  return new_color;
}

// World to raster/screen space
const Point2D Rasterizer::rasterize(const Point3D& point_world) const {
  const Point3D point_camera = m_camera->viewTransform(point_world);
  const Point2D point_ndc = m_camera->projectTransform(point_camera);
  //const Point2D point_ = m_camera->ndcTransform(point_ndc);
  const Point2D point_raster = m_camera->viewportTransform(point_ndc);  
  return point_raster;
}

// World to raster/screen space
const Triangle2D Rasterizer::rasterize(const Triangle3D& triangle_world) const {
  return Triangle2D(
    rasterize(triangle_world.v1.position),
    rasterize(triangle_world.v2.position),
    rasterize(triangle_world.v3.position)
  );
}


// Raster/screen space to world
const Point3D Rasterizer::unrasterize(const Point2D& point_raster, const double depth_from_camera) const {
  const Point2D point_ndc = m_camera->viewportTransformInv(point_raster);
  const Point3D point_camera = m_camera->projectTransformInv(point_ndc, depth_from_camera);
  const Point3D point_world= m_camera->viewTransformInv(point_camera);
  return point_world;
}

void Rasterizer::exportDepthBuffer(const std::vector<double>& depth_buffer, const std::string output_path, const uint16_t image_width, const uint16_t image_height) const {
  std::vector<RGBColor> depth_buffer_grey(image_height*image_width, RGBColor(1.0));
  for (int i = 0; i < depth_buffer.size(); ++i) {
    const double depth = depth_buffer[i];
    if (depth != m_camera->get_far_plane()) {
      // Convert depth in the range [near, far] to [0,1]
      const double slope = 1.0 / (m_camera->get_far_plane() - m_camera->get_near_plane());
      const double depth_normalized = slope * (depth - m_camera->get_near_plane());
      depth_buffer_grey[i] = RGBColor(depth_normalized);
    }

  }
  exportImage(depth_buffer_grey, output_path, image_width, image_height);
}

// Build a shadow map for each light
const void Rasterizer::createShadowMaps() {
  for (auto& light : m_world->m_lights) {
    ShadowMap * shadow_map = new ShadowMap(m_world, light);
    shadow_map->render();
    m_shadow_maps.push_back(shadow_map);
  }
}

// Calculate shadows using shadow maps
const double Rasterizer::shadowFactor(const Point3D& point_world) {
  double shadow = 0.0;         // No shadow     
  for (auto& shadow_map : m_shadow_maps) {
    shadow += shadow_map->pointInShadow(point_world);
  }
  return shadow;
}