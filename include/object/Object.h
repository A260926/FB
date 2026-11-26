#ifndef OBJECT_H
#define OBJECT_H

#include "util/Utilities.h"
#include "bvh/BoundingBox.h"
#include "Material.h"
#include <algorithm>
#include <cstdint>

// Default layer set: every object is visible and every ray queries every layer.
constexpr std::uint32_t VISIBILITY_MASK_ALL = 0xffffffffu;

// An object is present for a ray only when their masks share at least one bit.
// A zero mask on either side never matches, so the zero query layer sees nothing.
inline bool visibility_matches(std::uint32_t object_mask, std::uint32_t ray_mask) {
    return (object_mask & ray_mask) != 0;
}



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

    // Visibility layer of the object. Objects on a layer that does not overlap
    // the ray's query mask are skipped as if they did not exist.
    // Virtual so BVH nodes report their cached aggregate OR through Object*.
    void set_visibility_mask(std::uint32_t mask) { visibilityMask = mask; }
    virtual std::uint32_t visibility_mask() const { return visibilityMask; }

  protected:
    std::uint32_t visibilityMask = VISIBILITY_MASK_ALL;
};

#endif