#include "Rasterizer.h"
#include "OrthographicCamera.h"
#include "PERSPECTIVECAMERA.H"
#include "ForwardRasterizer.h"
#include "GeometryObject.h"
#include <stdexcept>

Rasterizer::Rasterizer()
  : Renderer() {

}

Rasterizer::Rasterizer(const World* world)
  : Renderer(world) {

}

Rasterizer::~Rasterizer() {
}

/******************************** Sectioning ********************************/

void Rasterizer::enableSection(const Vector3D& n, const double d,
                               const RGBColor& color, const RGBColor& diffuse,
                               const RGBColor& specular) {
  m_section.enable(n, d, color, diffuse, specular);
}

void Rasterizer::updateSection(const Vector3D& n, const double d,
                               const RGBColor& color, const RGBColor& diffuse,
                               const RGBColor& specular) {
  m_section.update(n, d, color, diffuse, specular);
}

void Rasterizer::disableSection() {
  m_section.disable();
}

bool Rasterizer::sectionEnabled() const {
  return m_section.enabled();
}

Vector3D Rasterizer::sectionPlaneNormal() const {
  return m_section.plane().n;
}

double Rasterizer::sectionPlaneD() const {
  return m_section.plane().d;
}

const std::vector<unsigned char>& Rasterizer::sectionCapMask() const {
  return m_section.capMask();
}

const std::vector<int32_t>& Rasterizer::sectionCapObject() const {
  return m_section.capObject();
}

bool Rasterizer::sectionKeeps(const Point3D& point) const {
  const SectionPlane& plane = m_section.plane();
  return plane.n * Vector3D(point) + plane.d >= 0.0;
}

Vector3D Rasterizer::sectionCapNormal() const {
  const Vector3D n = m_section.plane().n;
  return Vector3D(-n.x, -n.y, -n.z);
}

Fragment Rasterizer::sectionCapFragment(const Point3D& point) const {
  const SectionPlane& plane = m_section.plane();
  return Fragment(point, plane.color, plane.diffuse, plane.specular,
                  sectionCapNormal());
}

// Depth of a covered pixel. Perspective keeps the production inverse axis
// depth reconstruction; orthographic interpolates the axis depth linearly.
// With sectioning disabled the legacy calculateDepth path is preserved.
double Rasterizer::rasterSampleDepth(const Triangle3D& triangle_world,
                                     const Triangle2D& triangle_raster,
                                     const Point2D& pixel_raster) const {
  if (!m_section.enabled()) {
    return calculateDepth(triangle_world, triangle_raster, pixel_raster);
  }

  const double z1 = m_camera->viewTransform(triangle_world.v1.position).z;
  const double z2 = m_camera->viewTransform(triangle_world.v2.position).z;
  const double z3 = m_camera->viewTransform(triangle_world.v3.position).z;

  double u, v, w;
  triangle_raster.calculateBarycentricCoords(u, v, w, pixel_raster);

  if (dynamic_cast<const OrthographicCamera*>(m_camera) != nullptr) {
    return u * z1 + v * z2 + w * z3;
  }
  return 1.0 / (u / z1 + v / z2 + w / z3);
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