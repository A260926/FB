#include "SectionPass.h"

#include <cmath>
#include <stdexcept>

#include "Camera.h"
#include "Point3D.h"

namespace {
bool finiteVector(const Vector3D& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finiteColor(const RGBColor& c) {
  return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b);
}
} // namespace

SectionPlane::SectionPlane()
  : n(Vector3D(0.0, 0.0, 1.0)), d(0.0),
    color(RGBColor(1.0f, 1.0f, 1.0f)),
    diffuse(RGBColor(1.0f, 1.0f, 1.0f)),
    specular(RGBColor(1.0f, 1.0f, 1.0f)) {
}

double sectionEdgeE(const Point2D& a, const Point2D& b, const Point2D& p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

SectionPlane makeSectionPlane(const Vector3D& n, double d,
                              const RGBColor& color, const RGBColor& diffuse,
                              const RGBColor& specular) {
  if (!finiteVector(n) || !std::isfinite(d) ||
      !finiteColor(color) || !finiteColor(diffuse) || !finiteColor(specular)) {
    throw std::invalid_argument("section plane configuration contains a non finite value");
  }

  const double length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  if (length == 0.0) {
    throw std::invalid_argument("section plane normal must not be the zero vector");
  }

  SectionPlane plane;
  plane.n = Vector3D(n.x / length, n.y / length, n.z / length);
  plane.d = d / length;
  plane.color = color;
  plane.diffuse = diffuse;
  plane.specular = specular;
  return plane;
}

bool sectionRayPlane(const SectionPlane& plane, const Camera& camera,
                     const Point2D& pixel, Point3D& intersection,
                     double& axis_depth) {
  // Point of the pixel ray at unit camera axis depth.
  const Point2D ndc = camera.viewportTransformInv(pixel);
  const Point3D ray_point_camera = camera.projectTransformInv(ndc, 1.0);
  const Point3D ray_point_world = camera.viewTransformInv(ray_point_camera);
  // viewDirection is the pixel ray direction for a perspective camera and the
  // constant view axis for an orthographic camera.
  const Vector3D direction = camera.viewDirection(ray_point_world);

  const double denominator = plane.n * direction;
  if (!std::isfinite(denominator) || std::abs(denominator) <= 1e-12) {
    return false; // Ray parallel to the section plane.
  }

  const double numerator = plane.n * Vector3D(ray_point_world) + plane.d;
  const double t = -numerator / denominator;
  if (!std::isfinite(t)) {
    return false;
  }

  const Point3D hit = ray_point_world + direction * t;
  if (!std::isfinite(hit.x) || !std::isfinite(hit.y) || !std::isfinite(hit.z)) {
    return false;
  }

  const double depth = camera.viewTransform(hit).z;
  if (!std::isfinite(depth) ||
      depth < camera.get_near_plane() || depth > camera.get_far_plane()) {
    return false;
  }

  intersection = hit;
  axis_depth = depth;
  return true;
}

SectionPass::SectionPass()
  : m_enabled(false), m_plane(), m_cap_mask(), m_cap_object() {
}

void SectionPass::enable(const Vector3D& n, double d, const RGBColor& color,
                         const RGBColor& diffuse, const RGBColor& specular) {
  // Build first: a throw leaves the existing configuration untouched.
  const SectionPlane next = makeSectionPlane(n, d, color, diffuse, specular);
  m_plane = next;
  m_enabled = true;
}

void SectionPass::update(const Vector3D& n, double d, const RGBColor& color,
                         const RGBColor& diffuse, const RGBColor& specular) {
  if (!m_enabled) {
    throw std::logic_error("section pass must be enabled before updating the plane");
  }
  enable(n, d, color, diffuse, specular);
}

void SectionPass::disable() {
  m_enabled = false;
  clearFrame();
}

bool SectionPass::enabled() const {
  return m_enabled;
}

const SectionPlane& SectionPass::plane() const {
  return m_plane;
}

void SectionPass::beginFrame(std::size_t pixel_count) {
  m_cap_mask.assign(pixel_count, 0);
  m_cap_object.assign(pixel_count, INVALID_OBJECT);
}

void SectionPass::setCap(std::size_t pixel_index, int32_t object_index) {
  m_cap_mask[pixel_index] = 1;
  m_cap_object[pixel_index] = object_index;
}

void SectionPass::setSurface(std::size_t pixel_index) {
  // A regular surface that wins the depth competition hides any cap that was
  // tentatively stored for this pixel.
  m_cap_mask[pixel_index] = 0;
  m_cap_object[pixel_index] = INVALID_OBJECT;
}

void SectionPass::clearFrame() {
  m_cap_mask.clear();
  m_cap_object.clear();
}

const std::vector<unsigned char>& SectionPass::capMask() const {
  return m_cap_mask;
}

const std::vector<int32_t>& SectionPass::capObject() const {
  return m_cap_object;
}
