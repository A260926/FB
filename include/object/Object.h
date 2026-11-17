#ifndef OBJECT_H
#define OBJECT_H

#include "util/Utilities.h"
#include "bvh/BoundingBox.h"
#include "Material.h"
#include <algorithm>
#include <cstdint>



class IntersectionRecord {
  // Record the information of the intersection point
  public:
    Point3 p{0,0,0};         // Intersection point coordinate
    Vector3 normal{0,0,0};   // Normal vector at the intersection point
    double t = 0;            // Ray parameter at the intersection point
    bool front_face = false; // True if the ray hits the front face of the object

    double u = 0, v = 0; // UV coordinates for texture mapping

    Material* mat_ptr = nullptr; // Pointer to the material of the object

    void set_face_normal(const Ray& r, const Vector3& outward_normal) {
        // Sets the hit record normal vector.
        front_face = dot(r.direction(), outward_normal) < 0;
        // If the ray hits the back face, flip the normal
        normal = front_face ? outward_normal : -outward_normal;
    }

};

class Object {
  // Base class for all objects in the scene
  public:
    virtual ~Object() = default;

    // Check if the ray intersects the object
    virtual bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const = 0;

    // Return the bounding box of the object
    virtual BoundingBox bounding_box() const = 0;

    // Check if the object should be excluded from the BVH
    virtual bool exclude_from_bvh() const { return false; }

    // Visibility layers this object belongs to. A ray only sees the object
    // when (ray.visibility_mask() & visibility_mask()) != 0; a zero result
    // means the object is treated as if it does not exist for that ray.
    void set_visibility_mask(std::uint32_t mask) { visibilityMask = mask; }
    std::uint32_t visibility_mask() const { return visibilityMask; }

  private:
    std::uint32_t visibilityMask = 0xffffffffu;
};

#endif