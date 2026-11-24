#include "SectionPass.h"

#include <cmath>
#include <stdexcept>

#include "Camera.h"
#include "OrthographicCamera.h"
#include "PERSPECTIVECAMERA.H"

namespace {

bool finiteColor(const RGBColor& color) {
  return std::isfinite(color.r) && std::isfinite(color.g) &&
         std::isfinite(color.b);
}

// Edge function E(a, b, p) = (b.x - a.x) * (p.y - a.y)
//                          - (b.y - a.y) * (p.x - a.x)
double edgeFunction(const Point2D& a, const Point2D& b, const Point2D& p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

}  // namespace

SectionConfig::SectionConfig()
  : n(0.0, 0.0, 1.0), d(0.0), color(1.0f), diffuse(1.0f), specular(1.0f) {
}

SectionConfig::SectionConfig(const Vector3D& normal, const double plane_d,
                             const RGBColor& cap_color,
                             const RGBColor& cap_diffuse,
                             const RGBColor& cap_specular)
  : n(normal), d(plane_d), color(cap_color), diffuse(cap_diffuse),
    specular(cap_specular) {
}

SectionConfig section_pass::validateConfig(const Vector3D& normal,
                                           const double plane_d,
                                           const RGBColor& color,
                                           const RGBColor& diffuse,
                                           const RGBColor& specular) {
  if (!std::isfinite(normal.x) || !std::isfinite(normal.y) ||
      !std::isfinite(normal.z) || !std::isfinite(plane_d)) {
    throw std::invalid_argument(
      "Section plane normal and constant must be finite");
  }
  const double length = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                  normal.z * normal.z);
  if (length == 0.0) {
    throw std::invalid_argument("Section plane normal must not be zero");
  }
  if (!finiteColor(color) || !finiteColor(diffuse) ||
      !finiteColor(specular)) {
    throw std::invalid_argument("Section cap colors must be finite");
  }

  SectionConfig config;
  config.n = normal / length;
  config.d = plane_d / length;
  config.color = color;
  config.diffuse = diffuse;
  config.specular = specular;
  return config;
}

PreparedTriangle section_pass::prepareTriangle(
    const Triangle2D& triangle_raster) {
  PreparedTriangle prepared;
  prepared.a = triangle_raster.v1;
  prepared.b = triangle_raster.v2;
  prepared.c = triangle_raster.v3;

  const double area_twice = edgeFunction(prepared.a, prepared.b, prepared.c);
  prepared.front = area_twice >= 0.0;
  prepared.degenerate = area_twice == 0.0;
  if (prepared.degenerate) {
    return prepared;
  }

  // Reorder until every edge keeps the triangle interior on its positive
  // half-plane, so a single strict edge rule covers every interior pixel.
  for (int iteration = 0; iteration < 3; ++iteration) {
    const double e_ab = edgeFunction(prepared.a, prepared.b, prepared.c);
    const double e_bc = edgeFunction(prepared.b, prepared.c, prepared.a);
    const double e_ca = edgeFunction(prepared.c, prepared.a, prepared.b);
    if (e_ab > 0.0 && e_bc > 0.0 && e_ca > 0.0) {
      break;
    }
    const Point2D temp = prepared.b;
    prepared.b = prepared.c;
    prepared.c = temp;
  }
  return prepared;
}

bool section_pass::triangleContains(const PreparedTriangle& triangle,
                                    const Point2D& pixel) {
  if (triangle.degenerate) {
    return false;
  }

  const Point2D edges[3][2] = {
    {triangle.a, triangle.b},
    {triangle.b, triangle.c},
    {triangle.c, triangle.a}
  };
  for (const auto& edge : edges) {
    const double e = edgeFunction(edge[0], edge[1], pixel);
    if (e > 0.0) {
      continue;
    }
    if (e < 0.0) {
      return false;
    }
    // Shared edges are counted once: only a top or left edge includes the
    // pixels lying on it (dy > 0, or dy == 0 with dx < 0).
    const double dx = edge[1].x - edge[0].x;
    const double dy = edge[1].y - edge[0].y;
    if (!(dy > 0.0 || (dy == 0.0 && dx < 0.0))) {
      return false;
    }
  }
  return true;
}

bool section_pass::rayPlaneHit(const Camera& camera,
                               const Point2D& pixel_raster,
                               const SectionConfig& config,
                               Point3D& hit_world, double& hit_depth) {
  const Point2D point_ndc = camera.viewportTransformInv(pixel_raster);

  if (dynamic_cast<const PerspectiveCamera*>(&camera) != nullptr) {
    // Ray through the pixel center in camera space.
    const Point3D far_camera =
      camera.projectTransformInv(point_ndc, camera.get_far_plane());
    const Point3D far_world = camera.viewTransformInv(far_camera);
    const Point3D origin = Point3D(
      camera.viewTransformInv(Point3D(0.0, 0.0, 0.0)));
    const Vector3D direction = far_world - origin;
    const double denominator = config.n * direction;
    if (denominator == 0.0) {
      return false;  // Ray parallel to the section plane.
    }
    const double t = -(config.n * Vector3D(origin) + config.d) / denominator;
    if (t <= 0.0) {
      return false;
    }
    const Point3D hit = origin + direction * t;
    const Point3D hit_camera = camera.viewTransform(hit);
    const double depth = hit_camera.z;
    if (depth < camera.get_near_plane() || depth > camera.get_far_plane()) {
      return false;
    }
    hit_world = hit;
    hit_depth = depth;
    return true;
  }

  // Orthographic camera: all rays share the camera view direction, parameter
  // values are axis depths directly.
  const Point3D near_camera =
    camera.projectTransformInv(point_ndc, camera.get_near_plane());
  const Point3D near_world = camera.viewTransformInv(near_camera);
  Vector3D direction = camera.viewDirection(near_world);
  direction.normalize();
  const double denominator = config.n * direction;
  if (denominator == 0.0) {
    return false;
  }
  const double t = -(config.n * Vector3D(near_world) + config.d) / denominator;
  if (t <= 0.0) {
    return false;
  }
  const Point3D hit = near_world + direction * t;
  const double depth = camera.get_near_plane() + t;
  if (depth > camera.get_far_plane()) {
    return false;
  }
  hit_world = hit;
  hit_depth = depth;
  return true;
}
