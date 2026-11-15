#ifndef SPHERE_H
#define SPHERE_H

#include "util/Utilities.h"
#include "object/Object.h"


// Child class of Object
// Represents a sphere object in the scene
class Sphere : public Object {
  public:

    inline static thread_local uint64_t hit_call_count = 0; // Number of times the intersection test was called

    Sphere(const Point3& center, double radius, Material* mat_ptr, bool exclude_from_bvh = false)
     : center(center), radius(std::fmax(0,radius)), mat_ptr(mat_ptr), excluded(exclude_from_bvh) {}

    bool exclude_from_bvh() const override { return excluded; }

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override {
        ++hit_call_count;
        
        // Calculate the intersection of the ray with the sphere
        Vector3 oc = center - r.origin();
        auto a = r.direction().length_squared();
        auto h = dot(r.direction(), oc);
        auto c = oc.length_squared() - radius*radius;
        // Discriminant of the quadratic equation
        auto discriminant = h*h - a*c;
        if (discriminant < 0)
            return false;

        auto sqrtd = std::sqrt(discriminant);

        // Find the nearest root that lies in the acceptable range.
        auto root1 = (h - sqrtd) / a;
        auto root2 = (h + sqrtd) / a;

        if (ray_t.surrounds(root1)) {
            rec.t = root1;
        } else if (ray_t.surrounds(root2)) {
            rec.t = root2;
        } else {
            return false;
        }

        rec.p = r.at(rec.t);
        Vector3 outward_normal = (rec.p - center) / radius;
        rec.set_face_normal(r, outward_normal);

        // Set the matreial pointer in hit_record
        rec.mat_ptr = mat_ptr;

        // Compute UV coordinates for texture mapping
        if (mat_ptr->has_texture) {
            uv_map(outward_normal, rec.u, rec.v);
        }
        return true;
    }

    BoundingBox bounding_box() const override {
        return BoundingBox(center - Vector3(radius, radius, radius), center + Vector3(radius, radius, radius));
    }

  private:
    Point3 center;  // Center of the sphere
    double radius;  // Radius of the sphere
    Material* mat_ptr;

    bool excluded;  // Wheter the sphere should be excluded from the BVH

    void uv_map(Vector3 outward_normal, double& u, double& v ) const{
        // Calculate base UV coordinates (equirectangular mapping)
        double base_u = 0.5 + std::atan2(outward_normal.z(), outward_normal.x()) / (2 * pi);
        double base_v = 1.0 - std::asin(outward_normal.y()) / pi;

        // Adjust for texture scaling and tiling
        double scale_factor_u = 1.0;
        double scale_factor_v = 1.0;

        if (mat_ptr->texture_width > 0 && mat_ptr->texture_height > 0) {
            // Compute aspect ratio scaling (if needed)
            double texture_aspect_ratio = double(mat_ptr->texture_width) / mat_ptr->texture_height;
            double sphere_aspect_ratio = 2.0; // Ideal aspect ratio for a sphere's UV map

            scale_factor_u = sphere_aspect_ratio / texture_aspect_ratio;
        }

        // Apply tiling and scaling
        double tiled_u = fmod(base_u * mat_ptr->tile_factor_u * scale_factor_u, 1.0);
        double tiled_v = fmod(base_v * mat_ptr->tile_factor_v * scale_factor_v, 1.0);

        // Ensure UVs remain in the [0, 1] range
        u = tiled_u >= 0 ? tiled_u : tiled_u + 1;
        v = tiled_v >= 0 ? tiled_v : tiled_v + 1;
    }

};

#endif