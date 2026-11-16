#include "Rasterizer.h"
#include "OrthographicCamera.h"
#include "ForwardRasterizer.h"

Rasterizer::Rasterizer()
  : Renderer() {

}

Rasterizer::Rasterizer(const World* world)
  : Renderer(world) {

}

Rasterizer::~Rasterizer() {
}

// Geometry pass for one thin-lens pupil. The renderer camera is the pupil
// camera, so every inherited transform (rasterize, calculateDepth,
// unrasterize, fragment interpolation) resolves through that same camera.
// Coverage and the depth test are recomputed here for every pupil: nothing is
// shared between samples.
void Rasterizer::rasterizePupilGeometry(const Camera& pupil_camera, PupilGbuffer& gbuffer) const {
  const uint32_t image_width = pupil_camera.get_width();
  const uint32_t image_height = pupil_camera.get_height();
  const uint32_t pixel_count = image_width * image_height;
  const double far_plane = pupil_camera.get_far_plane();

  gbuffer.depth.assign(pixel_count, far_plane);
  gbuffer.fragment.assign(pixel_count, Fragment());
  gbuffer.hit.assign(pixel_count, 0);

  for (auto& object : m_world->m_objects) {
    const std::vector<Triangle3D> triangles = object->triangles();
    for (auto& triangle_world : triangles) {
      const Triangle2D triangle_raster = this->rasterize(triangle_world);
      const BoundingBox2D bbox_raster = triangle_raster.bbox();
      for (int32_t pixel_raster_x = bbox_raster.min.x; pixel_raster_x <= bbox_raster.max.x; ++pixel_raster_x) {
        for (int32_t pixel_raster_y = bbox_raster.min.y; pixel_raster_y <= bbox_raster.max.y; ++pixel_raster_y) {
          const Point2D pixel_raster = { (double)pixel_raster_x, (double)pixel_raster_y };
          if (triangle_raster.contains(pixel_raster)) {
            const double depth = calculateDepth(triangle_world, triangle_raster, pixel_raster);
            const Point3D pixel_world = unrasterize(pixel_raster, depth);
            if (pupil_camera.insideFrustrum(pixel_raster, depth)) {
              const uint32_t i = pixel_raster_y * image_width + pixel_raster_x;
              if (depth < gbuffer.depth[i]) {
                gbuffer.fragment[i] = calculateFragmentAttributes(triangle_world, pixel_world,
                                                                   triangle_raster, pixel_raster,
                                                                   *object->material());
                gbuffer.depth[i] = depth;
                gbuffer.hit[i] = 1;
              }
            }
          }
        }
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

// Independent center pinhole geometry draw. Reuses the shared geometry pass
// with the center camera (the renderer camera is the center camera when this
// is called), producing a depth buffer that is unrelated to any pupil sample.
std::vector<double> Rasterizer::renderCenterDepth(const Camera& center_camera) {
  PupilGbuffer gbuffer;
  rasterizePupilGeometry(center_camera, gbuffer);
  return gbuffer.depth;
}

// Exposes the original BMP export path for a frame accumulated outside the
// renderer (the weighted thin-lens average).
void Rasterizer::exportPupilImage(const std::vector<RGBColor>& pixels, const std::string output_path) const {
  exportImage(pixels, output_path,
              static_cast<uint16_t>(m_camera->get_width()),
              static_cast<uint16_t>(m_camera->get_height()));
}

// BMP export with caller-supplied dimensions (a thin-lens comparison frame).
void Rasterizer::exportImageSized(const std::vector<RGBColor>& pixels, const std::string output_path,
                                  uint16_t image_width, uint16_t image_height) const {
  exportImage(pixels, output_path, image_width, image_height);
}