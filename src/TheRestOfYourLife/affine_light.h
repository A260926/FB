#ifndef AFFINE_LIGHT_H
#define AFFINE_LIGHT_H
//==============================================================================================
// affine_instance: a reusable time-dependent affine wrapper around any hittable.
//
// The wrapper maps a local point p to a world point
//
//     x_world(t) = A * p + b(t),   b(t) = (1 - t) * b0 + t * b1,   t in [0, 1]
//
// where A is a finite, invertible 3x3 matrix (non-uniform scale, shear and negative
// determinants are all allowed) and b0, b1 are the translation endpoints over the shutter
// interval. Instances may be nested, and one underlying object may be shared by several
// instances: the object itself is never moved.
//
// The input matrix and endpoints are copied at construction. Non-finite or singular input is
// rejected outright.
//
// Matrix layout (see README.md): mat3 stores its entries row-major as e[row][column] and
// multiplies a column vector, so (A * v)[i] = sum_j A(i,j) * v[j].
//
// Light proposals reuse the local object's own random()/pdf_value() machinery at a single
// shutter time. The reported density is a true world-space solid-angle density, including the
// solid-angle Jacobian of the direction map d_world = A * d_local.
//==============================================================================================

#include "hittable.h"

#include <cmath>
#include <stdexcept>


class mat3 {
  public:
    // Row-major storage: e[row][column].
    double e[3][3];

    mat3() {
        e[0][0] = 1; e[0][1] = 0; e[0][2] = 0;
        e[1][0] = 0; e[1][1] = 1; e[1][2] = 0;
        e[2][0] = 0; e[2][1] = 0; e[2][2] = 1;
    }

    mat3(double m00, double m01, double m02,
         double m10, double m11, double m12,
         double m20, double m21, double m22)
    {
        e[0][0] = m00; e[0][1] = m01; e[0][2] = m02;
        e[1][0] = m10; e[1][1] = m11; e[1][2] = m12;
        e[2][0] = m20; e[2][1] = m21; e[2][2] = m22;
    }

    static mat3 from_columns(const vec3& c0, const vec3& c1, const vec3& c2) {
        return mat3(c0.x(), c1.x(), c2.x(),
                    c0.y(), c1.y(), c2.y(),
                    c0.z(), c1.z(), c2.z());
    }

    static mat3 from_rows(const vec3& r0, const vec3& r1, const vec3& r2) {
        return mat3(r0.x(), r0.y(), r0.z(),
                    r1.x(), r1.y(), r1.z(),
                    r2.x(), r2.y(), r2.z());
    }

    double operator()(int row, int col) const { return e[row][col]; }

    // Matrix times column vector.
    vec3 operator*(const vec3& v) const {
        return vec3(e[0][0]*v.x() + e[0][1]*v.y() + e[0][2]*v.z(),
                    e[1][0]*v.x() + e[1][1]*v.y() + e[1][2]*v.z(),
                    e[2][0]*v.x() + e[2][1]*v.y() + e[2][2]*v.z());
    }

    // Matrix times matrix.
    mat3 operator*(const mat3& m) const {
        mat3 result;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                double sum = 0.0;
                for (int k = 0; k < 3; k++)
                    sum += e[i][k] * m.e[k][j];
                result.e[i][j] = sum;
            }
        return result;
    }

    mat3 transpose() const {
        return mat3(e[0][0], e[1][0], e[2][0],
                    e[0][1], e[1][1], e[2][1],
                    e[0][2], e[1][2], e[2][2]);
    }

    double determinant() const {
        return e[0][0] * (e[1][1]*e[2][2] - e[1][2]*e[2][1])
             - e[0][1] * (e[1][0]*e[2][2] - e[1][2]*e[2][0])
             + e[0][2] * (e[1][0]*e[2][1] - e[1][1]*e[2][0]);
    }

    // Throws std::invalid_argument when the matrix is non-finite or singular.
    mat3 inverse() const {
        double det = determinant();
        double scale = 1.0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                if (!std::isfinite(e[i][j]))
                    throw std::invalid_argument("mat3::inverse: non-finite entry");
                scale = std::fmax(scale, std::fabs(e[i][j]));
            }
        if (!std::isfinite(det) || std::fabs(det) <= 1e-12 * scale*scale*scale)
            throw std::invalid_argument("mat3::inverse: singular matrix");

        double inv_det = 1.0 / det;
        // Cofactor/adjugate expansion.
        return mat3(
            (e[1][1]*e[2][2] - e[1][2]*e[2][1]) * inv_det,
            (e[0][2]*e[2][1] - e[0][1]*e[2][2]) * inv_det,
            (e[0][1]*e[1][2] - e[0][2]*e[1][1]) * inv_det,
            (e[1][2]*e[2][0] - e[1][0]*e[2][2]) * inv_det,
            (e[0][0]*e[2][2] - e[0][2]*e[2][0]) * inv_det,
            (e[0][2]*e[1][0] - e[0][0]*e[1][2]) * inv_det,
            (e[1][0]*e[2][1] - e[1][1]*e[2][0]) * inv_det,
            (e[0][1]*e[2][0] - e[0][0]*e[2][1]) * inv_det,
            (e[0][0]*e[1][1] - e[0][1]*e[1][0]) * inv_det
        );
    }
};


class affine_instance : public hittable {
  public:
    affine_instance(shared_ptr<hittable> object, const mat3& linear,
                    const vec3& b_endpoint0, const vec3& b_endpoint1)
      : object(object), A(linear), b0(b_endpoint0), b1(b_endpoint1)
    {
        if (!object)
            throw std::invalid_argument("affine_instance: null object");
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                if (!std::isfinite(A.e[i][j]))
                    throw std::invalid_argument("affine_instance: non-finite matrix entry");
        if (!all_finite(b0) || !all_finite(b1))
            throw std::invalid_argument("affine_instance: non-finite translation endpoint");

        // inverse() rejects a singular (or non-finite) linear part.
        A_inv = A.inverse();
        A_inv_transpose = A_inv.transpose();
        det_A = A.determinant();

        bbox = compute_bounding_box();
    }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        check_time(r.time());

        // Transform the whole ray with the same shutter time. The local ray parameter is
        // identical to the world one, so a non-normalized world direction still yields a t for
        // which r.at(t) is the world hit point:
        //     A*(o' + t*d') + b(t) = A*A^-1*(o - b) + t*A*A^-1*d + b(t) = o + t*d.
        vec3 b = b_at(r.time());
        vec3 local_origin = A_inv * (r.origin() - b);
        vec3 local_direction = A_inv * r.direction();
        ray local_r(local_origin, local_direction, r.time());

        if (!object->hit(local_r, ray_t, rec))
            return false;

        vec3 local_p = rec.p;

        // Recover the local outward normal (the stored normal faces the local ray) and map it
        // through the inverse transpose. The local object's exterior side is authoritative, so
        // a negative determinant never swaps inside and outside.
        vec3 local_outward = rec.front_face ? rec.normal : -rec.normal;
        vec3 world_outward = A_inv_transpose * local_outward;
        if (world_outward.near_zero() || !all_finite(world_outward))
            return false;
        world_outward = unit_vector(world_outward);

        rec.p = A * local_p + b;
        // Rebuild the front-face flag against the world ray; no winding-based extra flip.
        rec.set_face_normal(r, world_outward);
        // Material, UV and time are carried over unchanged.
        return true;
    }

    aabb bounding_box() const override { return bbox; }

    // Legacy time-less entry points: behave exactly as the time-aware ones at time = 0.
    double pdf_value(const point3& origin, const vec3& direction) const override {
        return pdf_value(origin, direction, 0.0);
    }

    vec3 random(const point3& origin) const override {
        return random(origin, 0.0);
    }

    double pdf_value(const point3& origin, const vec3& direction, double time) const override {
        check_time(time);
        if (!all_finite(origin) || !all_finite(direction) || direction.near_zero())
            return 0.0;

        vec3 b = b_at(time);
        vec3 local_origin = A_inv * (origin - b);
        vec3 local_direction = A_inv * direction;
        if (local_direction.near_zero() || !all_finite(local_direction))
            return 0.0;

        double local_density = object->pdf_value(local_origin, local_direction, time);

        // Solid-angle Jacobian of d_world = A * d_local:
        //     d(omega_world) / d(omega_local)
        //         = |det A| * |d_local|^3 / |d_world|^3.
        // Scaling the query direction by any positive factor leaves the ratio unchanged, and
        // the absolute value keeps the density non-negative under a negative determinant.
        double ratio = local_direction.length() / direction.length();
        double jacobian = std::fabs(det_A) * ratio*ratio*ratio;
        if (!(jacobian > 0.0))
            return 0.0;

        return local_density / jacobian;
    }

    vec3 random(const point3& origin, double time) const override {
        check_time(time);
        if (!all_finite(origin))
            throw std::invalid_argument("affine_instance::random: non-finite origin");

        // Pull the observation point back into local space, ask the local object for a
        // proposal direction, and map that direction back into world space.
        vec3 b = b_at(time);
        vec3 local_origin = A_inv * (origin - b);
        vec3 local_direction = object->random(local_origin, time);
        return A * local_direction;
    }

  private:
    shared_ptr<hittable> object;
    mat3 A;
    mat3 A_inv;
    mat3 A_inv_transpose;
    vec3 b0, b1;
    double det_A;
    aabb bbox;

    static bool all_finite(const vec3& v) {
        return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
    }

    static void check_time(double time) {
        if (!std::isfinite(time) || time < 0.0 || time > 1.0)
            throw std::invalid_argument("affine_instance: time must lie in [0,1]");
    }

    vec3 b_at(double time) const {
        return (1.0 - time) * b0 + time * b1;
    }

    aabb compute_bounding_box() const {
        // Bounding box of A applied to the local box's eight corners, swept between the two
        // translation endpoints. The translation is linear in time, so the endpoint boxes
        // together cover the whole shutter interval.
        aabb local = object->bounding_box();
        point3 lo(local.x.min, local.y.min, local.z.min);
        point3 hi(local.x.max, local.y.max, local.z.max);

        point3 mn( infinity,  infinity,  infinity);
        point3 mx(-infinity, -infinity, -infinity);
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                for (int k = 0; k < 2; k++) {
                    vec3 corner(i ? hi.x() : lo.x(),
                                j ? hi.y() : lo.y(),
                                k ? hi.z() : lo.z());
                    vec3 mapped = A * corner;
                    for (int c = 0; c < 3; c++) {
                        mn[c] = std::fmin(mn[c], mapped[c]);
                        mx[c] = std::fmax(mx[c], mapped[c]);
                    }
                }
            }
        }

        aabb linear_box(mn, mx);
        return aabb(linear_box + b0, linear_box + b1);
    }
};


#endif
