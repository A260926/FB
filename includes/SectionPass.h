#ifndef SECTION_PASS_H
#define SECTION_PASS_H

// CPU world-space single plane sectioning support.
//
// The section pass is an image-only feature: it never modifies the triangle
// meshes, the materials or the World. A plane n.p + d = 0 splits the world and
// every fragment with n.p + d >= 0 is kept. The solid part exposed by the cut
// is filled with cap fragments whose normal is -normalize(n) and whose colors
// come from the section configuration.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Point2D.h"
#include "RGBColor.h"
#include "Vector3D.h"

class Camera;

// Normalized section plane configuration. Stored by value: n is unit length
// and d is scaled together with n so that n.p + d is invariant under the
// normalization.
struct SectionPlane {
  Vector3D n;
  double d;
  RGBColor color;
  RGBColor diffuse;
  RGBColor specular;

  SectionPlane();
};

// Signed edge function used by the shared top-left coverage rule:
// E(a,b,p) = (b.x-a.x)*(p.y-a.y) - (b.y-a.y)*(p.x-a.x)
double sectionEdgeE(const Point2D& a, const Point2D& b, const Point2D& p);

// Builds a normalized SectionPlane. Throws std::invalid_argument when any
// value is non finite, when n is the zero vector or when one of the cap
// colors contains a non finite component. Nothing is mutated on failure.
SectionPlane makeSectionPlane(const Vector3D& n, double d,
                              const RGBColor& color, const RGBColor& diffuse,
                              const RGBColor& specular);

// Intersects the camera ray of an integer raster pixel with the section plane.
// Returns false when the ray is parallel to the plane or the intersection is
// not inside the camera near/far axis depth range. On success P holds the
// true world-space plane intersection and axis_depth the camera-space depth.
bool sectionRayPlane(const SectionPlane& plane, const Camera& camera,
                     const Point2D& pixel, Point3D& P, double& axis_depth);

class SectionPass {
public:
  static constexpr int32_t INVALID_OBJECT = -1;

  SectionPass();

  // Enable/replace the section plane. Invalid arguments throw
  // std::invalid_argument and leave the previous configuration untouched.
  void enable(const Vector3D& n, double d, const RGBColor& color,
              const RGBColor& diffuse, const RGBColor& specular);

  // Replace the plane of an already enabled section pass. Throws
  // std::logic_error when sectioning is disabled.
  void update(const Vector3D& n, double d, const RGBColor& color,
              const RGBColor& diffuse, const RGBColor& specular);

  // Disable sectioning and clear every per-frame readback buffer.
  void disable();

  bool enabled() const;
  const SectionPlane& plane() const;

  // Per-frame data, rebuilt on every section render.
  void beginFrame(std::size_t pixel_count);
  void setCap(std::size_t pixel_index, int32_t object_index);
  void setSurface(std::size_t pixel_index);
  void clearFrame();

  // 1 where a cap fragment actually won the depth competition, 0 elsewhere.
  const std::vector<unsigned char>& capMask() const;
  // Owning object index for visible cap pixels, INVALID_OBJECT elsewhere.
  const std::vector<int32_t>& capObject() const;

private:
  bool m_enabled;
  SectionPlane m_plane;
  std::vector<unsigned char> m_cap_mask;
  std::vector<int32_t> m_cap_object;
};

#endif // SECTION_PASS_H
