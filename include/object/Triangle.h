#ifndef TRIANGLE_H
#define TRIANGLE_H

#include "util/Utilities.h"
#include "object/Object.h"

class Triangle : public Object {
  public:

    inline static thread_local uint64_t hit_call_count = 0; // Number of times the intersection test was called

    Triangle(const Point3& v0, const Point3& v1, const Point3& v2, Material* mat_ptr,
             const Vector3& uv0 = Vector3(0,0,0), const Vector3& uv1= Vector3(1,0,0),
            const Vector3& uv2= Vector3(0,1,0), bool exclude_from_bvh = false)
        : v0(v0), v1(v1), v2(v2), mat_ptr(mat_ptr),
        uv0(uv0), uv1(uv1), uv2(uv2), excluded(exclude_from_bvh) {}

    bool exclude_from_bvh() const override { return excluded; }

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override {

        ++hit_call_count;

        // Using the Möller–Trumbore algorithm for ray-triangle intersection
        const Vector3 edge1 = v1 - v0;
        const Vector3 edge2 = v2 - v0;
        const Vector3 h = cross(r.direction(), edge2);
        const double a = dot(edge1, h);

        // If a is close to 0, the ray is parallel to the triangle
        constexpr double epsilon = 1e-8;
        if (std::fabs(a) < epsilon)
            return false;

        const double f = 1.0 / a;
        const Vector3 s = r.origin() - v0;
        const double u = f * dot(s, h);

        // Check if the intersection lies within the triangle
        if (u < 0.0 || u > 1.0)
            return false;

        const Vector3 q = cross(s, edge1);
        const double v = f * dot(r.direction(), q);

        if (v < 0.0 || u + v > 1.0)
            return false;

        // Compute t to find where the intersection occurs on the ray
        const double t = f * dot(edge2, q);

        if (!ray_t.surrounds(t))
            return false;

        // Fill hit record
        rec.t = t;
        rec.p = r.at(t);
        Vector3 outward_normal = unit_vector(cross(edge1, edge2)); // Normalize the normal
        rec.set_face_normal(r, outward_normal);

        // Set the material pointer in hit_record
        rec.mat_ptr = mat_ptr;

        if (mat_ptr->has_texture) {
            uv_map(u, v, rec.u, rec.v);
        }

        return true;
    }

    BoundingBox bounding_box() const override {
        // Calculate the bounding box of the triangle
        Point3 min = Point3(
            std::min({v0.x(), v1.x(), v2.x()}),
            std::min({v0.y(), v1.y(), v2.y()}),
            std::min({v0.z(), v1.z(), v2.z()})
        );
        Point3 max = Point3(
            std::max({v0.x(), v1.x(), v2.x()}),
            std::max({v0.y(), v1.y(), v2.y()}),
            std::max({v0.z(), v1.z(), v2.z()})
        );
        return BoundingBox(min, max);
    }

  private:
    Point3 v0, v1, v2; // Triangle vertices
    Material* mat_ptr;
    Vector3 uv0, uv1, uv2; // UV coordinates for texture mapping, ignore z componenet

    bool excluded; // Whether the triangle should be excluded from the BVH

    void uv_map(double u, double v, double& uv_u, double& uv_v ) const{
        // Interpolate UV coordinates using barycentric coordinates
        uv_u = (1 - u - v) * uv0.x() + u * uv1.x() + v * uv2.x();
        uv_v = (1 - u - v) * uv0.y() + u * uv1.y() + v * uv2.y();
    }
};

#endif