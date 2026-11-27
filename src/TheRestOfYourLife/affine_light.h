#ifndef AFFINE_LIGHT_H
#define AFFINE_LIGHT_H
//==============================================================================================
// affine_instance: a reusable, time-dependent affine wrapper around any hittable.
//
// A local point p is placed in world space at shutter time t in [0,1] as
//
//     world(p, t) = A * p + b(t),   b(t) = (1-t)*b0 + t*b1
//
// where A is a finite, invertible 3x3 matrix (non-uniform scaling, shearing and
// negative determinants are all allowed). The matrix and both translation
// endpoints are copied at construction; non-finite or singular inputs are
// rejected (the factory returns a null pointer and is_valid() is false).
//
// Wrappers may be nested and several instances may share one unmoved source
// object. Light proposals keep using the local object's own random()/pdf_value()
// distributions; only the ray/normal geometry and the solid-angle measure are
// mapped through A.
//==============================================================================================

#include "rtweekend.h"
#include "hittable.h"

#include <stdexcept>


class affine_instance : public hittable {
  public:
    // matrix layout: row-major 3x3, A[row][col], so (A*p)[i] = sum_j A[i][j]*p[j].
    affine_instance(shared_ptr<hittable> object,
                    const double matrix[3][3],
                    const point3& offset0,
                    const point3& offset1)
      : object(object), b0(offset0), b1(offset1), valid(false)
    {
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                A[i][j] = matrix[i][j];

        if (!finite_matrix(A) || !finite_point(b0) || !finite_point(b1))
            return;

        determinant = det3(A);
        if (!std::isfinite(determinant) || determinant == 0.0)
            return;

        // Inverse via the adjugate (cofactor transpose) divided by the determinant.
        inverse[0][0] =  (A[1][1]*A[2][2] - A[1][2]*A[2][1]) / determinant;
        inverse[0][1] =  (A[0][2]*A[2][1] - A[0][1]*A[2][2]) / determinant;
        inverse[0][2] =  (A[0][1]*A[1][2] - A[0][2]*A[1][1]) / determinant;
        inverse[1][0] =  (A[1][2]*A[2][0] - A[1][0]*A[2][2]) / determinant;
        inverse[1][1] =  (A[0][0]*A[2][2] - A[0][2]*A[2][0]) / determinant;
        inverse[1][2] =  (A[0][2]*A[1][0] - A[0][0]*A[1][2]) / determinant;
        inverse[2][0] =  (A[1][0]*A[2][1] - A[1][1]*A[2][0]) / determinant;
        inverse[2][1] =  (A[0][1]*A[2][0] - A[0][0]*A[2][1]) / determinant;
        inverse[2][2] =  (A[0][0]*A[1][1] - A[0][1]*A[1][0]) / determinant;

        if (!finite_matrix(inverse))
            return;

        valid = true;
        bbox = compute_bounding_box();
    }

    // Returns a null pointer instead of constructing an invalid instance.
    static shared_ptr<affine_instance> make(shared_ptr<hittable> object,
                                            const double matrix[3][3],
                                            const point3& offset0,
                                            const point3& offset1) {
        auto instance = make_shared<affine_instance>(object, matrix, offset0, offset1);
        if (!instance->is_valid())
            return nullptr;
        return instance;
    }

    bool is_valid() const { return valid; }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        if (!valid)
            return false;

        // Transform the world ray into local space. The ray parameter is unchanged,
        // so a local hit at t satisfies the original world ray r.at(t) exactly, even
        // when r.direction() is not normalized.
        vec3 offset = offset_at(r.time());
        ray local_ray(multiply(inverse, r.origin() - offset),
                      multiply(inverse, r.direction()),
                      r.time());

        if (!object->hit(local_ray, ray_t, rec))
            return false;

        // Recover the geometric outward normal of the local object.
        vec3 local_outward = rec.front_face ? rec.normal : -rec.normal;

        // Point back to world space.
        rec.p = multiply(A, rec.p) + offset;

        // Normals use the inverse transpose; re-normalize after the linear map and
        // rebuild front_face from the world ray. The local object's outer side is
        // preserved, including when A has a negative determinant.
        vec3 world_outward = multiply_transpose(inverse, local_outward);
        rec.set_face_normal(r, unit_vector(world_outward));

        // Material, UV coordinates and time are carried through unchanged.
        return true;
    }

    aabb bounding_box() const override { return bbox; }

    // Time-less entry points: an affine_instance reached through the old interface
    // is evaluated at time = 0.
    double pdf_value(const point3& origin, const vec3& direction) const override {
        return pdf_value(origin, direction, 0.0);
    }

    vec3 random(const point3& origin) const override {
        return random(origin, 0.0);
    }

    double pdf_value(const point3& origin, const vec3& direction, double time)
    const override {
        if (!valid)
            return 0.0;
        require_time(time);

        // A zero or non-finite query direction carries no solid angle.
        if (!finite_point(direction) || direction.near_zero())
            return 0.0;

        vec3 offset = offset_at(time);
        point3 local_origin = multiply(inverse, origin - offset);
        vec3 local_direction = multiply(inverse, direction);

        // The local object keeps its own sampling preconditions (a sphere's origin
        // must be outside it, a quad's must be off its plane). Defensively treat any
        // non-finite or negative local answer as "no density here".
        double local_density = object->pdf_value(local_origin, local_direction, time);
        if (!std::isfinite(local_density) || local_density <= 0.0)
            return 0.0;

        // Solid-angle Jacobian for d_world = A * d_local:
        //     dOmega_world = |det A| * |d_local|^3 / |A d_local|^3 * dOmega_local
        // so the world density is the local density times the inverse factor. This
        // is invariant under positive rescaling of the query direction and stays
        // non-negative for negative determinants.
        auto local_len3 = local_direction.length_squared() * local_direction.length();
        auto world_len3 = direction.length_squared() * direction.length();
        if (!(local_len3 > 0.0) || !(world_len3 > 0.0))
            return 0.0;

        return local_density * world_len3 / (std::fabs(determinant) * local_len3);
    }

    vec3 random(const point3& origin, double time) const override {
        if (!valid)
            return vec3(0, 0, 0);
        require_time(time);

        vec3 offset = offset_at(time);
        point3 local_origin = multiply(inverse, origin - offset);

        // Keep the local object's own proposal distribution.
        vec3 local_direction = object->random(local_origin, time);
        if (!finite_point(local_direction))
            return vec3(0, 0, 0);

        return multiply(A, local_direction);
    }

  private:
    shared_ptr<hittable> object;
    double A[3][3];
    double inverse[3][3];
    double determinant = 0.0;
    point3 b0, b1;
    bool valid;
    aabb bbox;

    static bool finite_point(const vec3& v) {
        return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
    }

    static bool finite_matrix(const double m[3][3]) {
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                if (!std::isfinite(m[i][j]))
                    return false;
        return true;
    }

    static double det3(const double m[3][3]) {
        return m[0][0] * (m[1][1]*m[2][2] - m[1][2]*m[2][1])
             - m[0][1] * (m[1][0]*m[2][2] - m[1][2]*m[2][0])
             + m[0][2] * (m[1][0]*m[2][1] - m[1][1]*m[2][0]);
    }

    static vec3 multiply(const double m[3][3], const vec3& v) {
        return vec3(m[0][0]*v.x() + m[0][1]*v.y() + m[0][2]*v.z(),
                    m[1][0]*v.x() + m[1][1]*v.y() + m[1][2]*v.z(),
                    m[2][0]*v.x() + m[2][1]*v.y() + m[2][2]*v.z());
    }

    // Multiply by the transpose of m (used for inverse-transpose normals).
    static vec3 multiply_transpose(const double m[3][3], const vec3& v) {
        return vec3(m[0][0]*v.x() + m[1][0]*v.y() + m[2][0]*v.z(),
                    m[0][1]*v.x() + m[1][1]*v.y() + m[2][1]*v.z(),
                    m[0][2]*v.x() + m[1][2]*v.y() + m[2][2]*v.z());
    }

    vec3 offset_at(double time) const {
        return (1.0 - time) * b0 + time * b1;
    }

    static void require_time(double time) {
        if (!std::isfinite(time) || time < 0.0 || time > 1.0)
            throw std::invalid_argument("affine_instance: shutter time must lie in [0,1]");
    }

    aabb compute_bounding_box() const {
        // The linear part is constant and the translation is linear in time, so the
        // swept box is bounded exactly by transforming the eight local box corners at
        // the two shutter endpoints.
        const aabb& local = object->bounding_box();
        point3 corners[8] = {
            point3(local.x.min, local.y.min, local.z.min),
            point3(local.x.min, local.y.min, local.z.max),
            point3(local.x.min, local.y.max, local.z.min),
            point3(local.x.min, local.y.max, local.z.max),
            point3(local.x.max, local.y.min, local.z.min),
            point3(local.x.max, local.y.min, local.z.max),
            point3(local.x.max, local.y.max, local.z.min),
            point3(local.x.max, local.y.max, local.z.max)
        };

        point3 first = multiply(A, corners[0]) + b0;
        aabb box(first, first);
        for (int k = 1; k < 8; k++)
            box = aabb(box, aabb(multiply(A, corners[k]) + b0,
                                 multiply(A, corners[k]) + b0));
        for (int k = 0; k < 8; k++)
            box = aabb(box, aabb(multiply(A, corners[k]) + b1,
                                 multiply(A, corners[k]) + b1));
        return box;
    }
};


#endif
