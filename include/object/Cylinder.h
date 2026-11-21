#ifndef CYLINDER_H
#define CYLINDER_H

#include "util/Utilities.h"
#include "object/Object.h"

class Cylinder : public Object {

  public:

  inline static thread_local uint64_t hit_call_count = 0;
    Cylinder(const Point3& center, const Vector3& axis, double radius, double height, Material* mat_ptr, bool exclude_from_bvh = false)
        : center(center), axis(unit_vector(axis)), radius(std::fmax(0, radius)),
         height(std::fmax(0, height)), mat_ptr(mat_ptr), excluded(exclude_from_bvh) {}

    bool exclude_from_bvh() const override { return excluded; }

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override {
        ++hit_call_count;

        IntersectionRecord temp_rec;
        bool hit_anything = false;
        auto closest_so_far = ray_t.max;
        // Check intersection with the side surface
        if (hit_cylinder_side(r, Interval(ray_t.min, closest_so_far), temp_rec)) {
            hit_anything = true;
            closest_so_far = temp_rec.t;
            rec = temp_rec; // Update the IntersectionRecord with the closest hit
        }
        // Check intersection with the top and bottom caps
        if (hit_cylinder_caps(r, Interval(ray_t.min, closest_so_far), temp_rec)) {
            hit_anything = true;
            closest_so_far = temp_rec.t;
            rec = temp_rec; // Update the IntersectionRecord with the closest hit
        }
        return hit_anything;
    }

  private:

    Point3 center;  // Center of the cylinder (midpoint along the height)
    Vector3 axis;      // Unit vector along the cylinder's axis
    double radius;  // Radius of the cylinder
    double height;  // Extent from center along axis (half-height; total height = 2 × height)
    Material* mat_ptr;

    bool excluded;

    // Check intersection with the cylinder's side surface
    bool hit_cylinder_side(const Ray& r, Interval ray_t, IntersectionRecord& rec) const {
        Vector3 oc = r.origin() - center;
        Vector3 d_proj = r.direction() - dot(r.direction(), axis) * axis;  // Direction perpendicular to axis
        Vector3 oc_proj = oc - dot(oc, axis) * axis;                      // Origin offset perpendicular to axis

        double a = d_proj.length_squared();
        if (a < 1e-12) return false;  // Ray parallel to cylinder axis
        double half_b = dot(d_proj, oc_proj);
        double c = oc_proj.length_squared() - radius * radius;

        double discriminant = half_b * half_b - a * c;
        if (discriminant < 0)
            return false;

        // Find the nearest root that lies in the acceptable range
        double sqrtd = std::sqrt(discriminant);
        double root = (-half_b - sqrtd) / a;

        if (!ray_t.surrounds(root)) {
            root = (-half_b + sqrtd) / a;
            if (!ray_t.surrounds(root))
                return false;
        }

        // Compute the intersection point and project onto the cylinder's axis
        rec.t = root;
        rec.p = r.at(rec.t);
        Vector3 to_p = rec.p - center;
        double axis_proj = dot(to_p, axis);  // Projection of the hit point along the axis

        // Check if the intersection is within the height of the cylinder
        double half_height = height;
        if (axis_proj < -half_height || axis_proj > half_height)
            return false;

        // Compute the normal at the intersection point
        Vector3 outward_normal = to_p - axis_proj * axis;  // Subtract the projection on the axis
        outward_normal = unit_vector(outward_normal);

        rec.set_face_normal(r, outward_normal);

        // Update the material in the hit record
        rec.mat_ptr = mat_ptr;

        if (mat_ptr->has_texture) {
            uv_map_side(outward_normal, axis_proj, rec.u, rec.v);
        }

        return true;
    }

    // Check intersection with the cylinder's top and bottom caps
    bool hit_cylinder_caps(const Ray& r, Interval ray_t, IntersectionRecord& rec) const {
        double half_height = height;
        Point3 top_center = center + half_height * axis;
        Point3 bottom_center = center - half_height * axis;

        // Check intersection with the top cap
        if (hit_disk(r, top_center, ray_t, rec, true))
            return true;

        // Check intersection with the bottom cap
        return hit_disk(r, bottom_center, ray_t, rec, false);
    }

    // Check intersection with a disk (cap)
    bool hit_disk(const Ray& r, const Point3& disk_center, Interval ray_t, IntersectionRecord& rec, bool is_top) const {
        Vector3 normal = is_top ? axis : -axis;
        double denom = dot(r.direction(), normal);
        if (fabs(denom) < 1e-12) return false;
        double t = dot(disk_center - r.origin(), normal) / denom;

        if (!ray_t.surrounds(t))
            return false;

        Point3 p = r.at(t);  // Intersection point
        if ((p - disk_center).length_squared() > radius * radius)
            return false;

        // Update hit record
        rec.t = t;
        rec.p = p;
        rec.set_face_normal(r, normal);

        // Assign the material pointer
        rec.mat_ptr = mat_ptr;

        // Compute UV for disk caps
        if (mat_ptr->has_texture) {
            uv_map_disk(p, disk_center, rec.u, rec.v);
        }

        return true;
    }

    BoundingBox bounding_box() const override {
        // Half-axis vector: height is the extent from center along the axis
        Vector3 half_axis = height * axis;

        // Calculate the top and bottom centers of the cylinder
        Point3 top_center = center + half_axis;
        Point3 bottom_center = center - half_axis;

        // A radius vector orthogonal to the cylinder's axis
        Vector3 radius_vector = radius * Vector3(1, 1, 1); // Uniform scaling in all directions

        // Adjust bounding box to cover the maximum extents
        // The bounding box must encompass the radius at all points
        Point3 min_point = Point3(
            std::fmin(top_center.x() - radius, bottom_center.x() - radius),
            std::fmin(top_center.y() - radius, bottom_center.y() - radius),
            std::fmin(top_center.z() - radius, bottom_center.z() - radius)
        );

        Point3 max_point = Point3(
            std::fmax(top_center.x() + radius, bottom_center.x() + radius),
            std::fmax(top_center.y() + radius, bottom_center.y() + radius),
            std::fmax(top_center.z() + radius, bottom_center.z() + radius)
        );

        return BoundingBox(min_point, max_point);
    }

    void uv_map_side(Vector3 outward_normal, double axis_proj, double& u, double& v ) const{
        // double theta = std::atan2(outward_normal.z(), outward_normal.x());
        // rec.u = 0.5 + theta / (2 * pi);
        // rec.v = 0.5 + axis_proj / height; // Map height proportionally to [0, 1]
        // Compute base UV coordinates
        double theta = std::atan2(outward_normal.z(), outward_normal.x());
        double base_u = 0.5 + theta / (2 * pi);  // Map to [0, 1]
        double base_v = 0.5 + axis_proj / height;  // Map height proportionally to [0, 1]

        // Dynamic scaling based on texture aspect ratio
        double ideal_aspect_ratio = (2.0 * pi * radius) / height;
        double texture_aspect_ratio = static_cast<double>(mat_ptr->texture_width) / mat_ptr->texture_height;

        double scale_factor_u = 1.0;
        double scale_factor_v = 1.0;

        if (texture_aspect_ratio > ideal_aspect_ratio) {
            scale_factor_v = texture_aspect_ratio / ideal_aspect_ratio;
        } else if (texture_aspect_ratio < ideal_aspect_ratio) {
            scale_factor_u = ideal_aspect_ratio / texture_aspect_ratio;
        }

        // Apply scaling and tiling

        double tiled_u = fmod(base_u * mat_ptr->tile_factor_u * scale_factor_u, 1.0);
        double tiled_v = fmod(base_v * mat_ptr->tile_factor_v * scale_factor_v, 1.0);

        // Ensure UV values are in the range [0, 1]
        u = tiled_u >= 0 ? tiled_u : tiled_u + 1;
        v = tiled_v >= 0 ? tiled_v : tiled_v + 1;
    }

    void uv_map_disk(Point3 p,const Point3& disk_center, double& u, double& v ) const{
        // Vector3 p_local = rec.p - disk_center; // Point relative to the disk center
        // rec.u = 0.5 + p_local.x() / (2 * radius);
        // rec.v = 0.5 + p_local.z() / (2 * radius);
        Vector3 p_local = p - disk_center; // Point relative to the disk center

        // Compute base UV coordinates
        double base_u = 0.5 + p_local.x() / (2 * radius);
        double base_v = 0.5 + p_local.z() / (2 * radius);

        double tiled_u = fmod(base_u * mat_ptr->tile_factor_u, 1.0);
        double tiled_v = fmod(base_v * mat_ptr->tile_factor_v, 1.0);

        // Ensure UV values are in the range [0, 1]
        u = tiled_u >= 0 ? tiled_u : tiled_u + 1;
        v = tiled_v >= 0 ? tiled_v : tiled_v + 1;
    }

};

#endif