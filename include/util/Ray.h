#ifndef RAY_H
#define RAY_H

#include "util/Vector3.h"

class Ray {
  public:
    Ray() : originPoint(0,0,0), vectorDirection(0,0,0), invDirection(0,0,0) {}

    Ray(const Point3& rayOrigin, const Vector3& rayDirection)
      : originPoint(rayOrigin), vectorDirection(rayDirection),
        invDirection(1.0/rayDirection[0], 1.0/rayDirection[1], 1.0/rayDirection[2]) {}

    Point3 at(double t) const {
      return  t*vectorDirection + originPoint;
    }

    const Point3& origin() const  { return originPoint; }
    const Vector3& direction() const { return vectorDirection; }
    const Vector3& inv_direction() const { return invDirection; }

  private:
    Point3 originPoint;
    Vector3 vectorDirection;
    Vector3 invDirection;
};

#endif