#ifndef SECTION_PASS_H
#define SECTION_PASS_H

// World-space single plane sectioning support.
//
// A section plane keeps the closed half-space  n . p + d >= 0.  The cut is a
// preview-only effect: triangle meshes are never modified and no cap geometry
// is exported.  Solid regions are decided with the non-zero integer winding
// number of each GeometryObject independently (never parity/odd-even).

#include <cstdint>
#include <vector>

#include "Point2D.h"
#include "Point3D.h"
#include "Vector3D.h"
#include "RGBColor.h"
#include "Triangle.h"

class Camera;

// Invalid value stored in the per-pixel cap ownership buffer for pixels that
// do not show a section cap in the current frame.
constexpr int32_t SECTION_INVALID_OBJECT = -1;

// Configuration of the section plane and of the cap material. Stored by value
// inside the Rasterizer. The normal is kept normalized together with d.
struct SectionConfig {
  Vector3D n;
  double d;
  RGBColor color;
  RGBColor diffuse;
  RGBColor specular;

  SectionConfig();
  SectionConfig(const Vector3D& normal, const double plane_d,
                const RGBColor& cap_color, const RGBColor& cap_diffuse,
                const RGBColor& cap_specular);
};

// One visible (winning) section cap for a single pixel.
struct SectionCap {
  Point3D position;
  Vector3D normal;
  double depth;
  uint32_t object_index;
  RGBColor color;
  RGBColor diffuse;
  RGBColor specular;

  SectionCap()
    : position(0.0), normal(0.0, 0.0, 1.0), depth(0.0), object_index(0),
      color(0.0f), diffuse(0.0f), specular(0.0f) {
  }
};

// A raster triangle reordered so that its interior satisfies E(a,b,p) > 0 for
// every edge. The original projected winding is remembered so it can set the
// crossing sign during winding accumulation.
struct PreparedTriangle {
  Point2D a, b, c;
  bool front;       // true when the original projection was counter clock wise
  bool degenerate;  // true when the triangle projects to zero area

  PreparedTriangle() : a(0.0), b(0.0), c(0.0), front(false), degenerate(true) {
  }
};

namespace section_pass {

// Validates a candidate configuration and returns the normalized one
// (n / |n|, d / |n|). Throws std::invalid_argument on non-finite values, a
// zero normal or non-finite color channels. The caller state is untouched
// until this returns.
SectionConfig validateConfig(const Vector3D& n, const double d,
                             const RGBColor& color, const RGBColor& diffuse,
                             const RGBColor& specular);

// Reorders a projected triangle for the shared top-left rule traversal used
// while sectioning is enabled.
PreparedTriangle prepareTriangle(const Triangle2D& triangle_raster);

// Directed coverage of a prepared triangle. A point on an edge is only
// included by the edge whose dy > 0, or dy == 0 and dx < 0.
bool triangleContains(const PreparedTriangle& triangle, const Point2D& pixel);

// Intersects the camera ray of an integer raster pixel with the section plane.
// Returns false (without writing the outputs) when the ray is parallel to the
// plane or the intersection falls outside the camera near/far range.
bool rayPlaneHit(const Camera& camera, const Point2D& pixel_raster,
                 const SectionConfig& config, Point3D& hit_world,
                 double& hit_depth);

}  // namespace section_pass

#endif  // SECTION_PASS_H
