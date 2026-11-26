#ifndef RAY_H
#define RAY_H

#include "util/Vector3.h"
#include <cstdint>

class Ray {
  public:
    Ray() : originPoint(0,0,0), vectorDirection(0,0,0), invDirection(0,0,0),
            visibilityMask(0xffffffffu) {}

    Ray(const Point3& rayOrigin, const Vector3& rayDirection,
        std::uint32_t mask = 0xffffffffu)
      : originPoint(rayOrigin), vectorDirection(rayDirection),
        invDirection(1.0/rayDirection[0], 1.0/rayDirection[1], 1.0/rayDirection[2]),
        visibilityMask(mask) {}

    Point3 at(double t) const {
      return  t*vectorDirection + originPoint;
    }

    const Point3& origin() const  { return originPoint; }
    const Vector3& direction() const { return vectorDirection; }
    const Vector3& inv_direction() const { return invDirection; }

    // Query layer carried by the ray. An object whose visibility mask has no
    // bit in common with this value is treated as not present.
    std::uint32_t visibility_mask() const { return visibilityMask; }

  private:
    Point3 originPoint;
    Vector3 vectorDirection;
    Vector3 invDirection;
    std::uint32_t visibilityMask;
};

#endif
