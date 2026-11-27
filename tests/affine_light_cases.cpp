//==============================================================================================
// Task tests for affine_instance: moving affine area lights with correct geometry,
// solid-angle density and camera transport. Registered through task_support/check.h;
// this file deliberately defines no main().
//==============================================================================================

#include "rtweekend.h"
#include "../task_support/check.h"

#include "affine_light.h"
#include "bvh.h"
#include "camera.h"
#include "hittable_list.h"
#include "material.h"
#include "pdf.h"
#include "quad.h"
#include "sphere.h"

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>


// ----------------------------------------------------------------------------------------------
// Small independent linear-algebra / sampling helpers (written independently of the class).
// ----------------------------------------------------------------------------------------------

static bool close(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps * (1.0 + std::fmax(std::fabs(a), std::fabs(b)));
}

static bool close_vec(const vec3& a, const vec3& b, double eps = 1e-9) {
    return close(a.x(), b.x(), eps) && close(a.y(), b.y(), eps) && close(a.z(), b.z(), eps);
}

static void mat_identity(double m[3][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] = (i == j) ? 1.0 : 0.0;
}

static double test_det(const double m[3][3]) {
    return m[0][0]*(m[1][1]*m[2][2] - m[1][2]*m[2][1])
         - m[0][1]*(m[1][0]*m[2][2] - m[1][2]*m[2][0])
         + m[0][2]*(m[1][0]*m[2][1] - m[1][1]*m[2][0]);
}

static vec3 test_mul(const double m[3][3], const vec3& v) {
    return vec3(m[0][0]*v.x()+m[0][1]*v.y()+m[0][2]*v.z(),
                m[1][0]*v.x()+m[1][1]*v.y()+m[1][2]*v.z(),
                m[2][0]*v.x()+m[2][1]*v.y()+m[2][2]*v.z());
}

static void test_inverse(const double m[3][3], double inv[3][3]) {
    double d = test_det(m);
    inv[0][0] =  (m[1][1]*m[2][2] - m[1][2]*m[2][1]) / d;
    inv[0][1] =  (m[0][2]*m[2][1] - m[0][1]*m[2][2]) / d;
    inv[0][2] =  (m[0][1]*m[1][2] - m[0][2]*m[1][1]) / d;
    inv[1][0] =  (m[1][2]*m[2][0] - m[1][0]*m[2][2]) / d;
    inv[1][1] =  (m[0][0]*m[2][2] - m[0][2]*m[2][0]) / d;
    inv[1][2] =  (m[0][2]*m[1][0] - m[0][0]*m[1][2]) / d;
    inv[2][0] =  (m[1][0]*m[2][1] - m[1][1]*m[2][0]) / d;
    inv[2][1] =  (m[0][1]*m[2][0] - m[0][0]*m[2][1]) / d;
    inv[2][2] =  (m[0][0]*m[1][1] - m[0][1]*m[1][0]) / d;
}

static vec3 test_mul_t(const double m[3][3], const vec3& v) {
    return vec3(m[0][0]*v.x()+m[1][0]*v.y()+m[2][0]*v.z(),
                m[0][1]*v.x()+m[1][1]*v.y()+m[2][1]*v.z(),
                m[0][2]*v.x()+m[1][2]*v.y()+m[2][2]*v.z());
}

static vec3 test_offset(const point3& b0, const point3& b1, double t) {
    return (1.0 - t) * b0 + t * b1;
}

// Independent sphere intersection in local space; returns false for a miss. The ray
// direction is intentionally left unnormalized.
static bool local_sphere_hit(const point3& origin, const vec3& direction,
                             const point3& center, double radius, double& t,
                             point3& local_point) {
    vec3 oc = center - origin;
    double a = direction.length_squared();
    double h = dot(direction, oc);
    double c = oc.length_squared() - radius * radius;
    double disc = h * h - a * c;
    if (disc < 0) return false;
    double s = std::sqrt(disc);
    double root = (h - s) / a;
    if (root < 0.001) {
        root = (h + s) / a;
        if (root < 0.001) return false;
    }
    t = root;
    local_point = origin + t * direction;
    return true;
}

static void sphere_uv_ref(const point3& unit_p, double& u, double& v) {
    double theta = std::acos(-unit_p.y());
    double phi = std::atan2(-unit_p.z(), unit_p.x()) + pi;
    u = phi / (2 * pi);
    v = theta / pi;
}

// Finite-difference gradient of F(w) = |A^-1 (w - b)|^2 - 1, i.e. an independent
// outward normal of the implicit ellipsoid surface.
static vec3 numeric_ellipsoid_normal(const double inv[3][3], const point3& b,
                                     const point3& w) {
    auto F = [&](const point3& q) {
        vec3 local = test_mul(inv, q - b);
        return local.length_squared() - 1.0;
    };
    double e = 1e-6;
    vec3 g(F(w + vec3(e, 0, 0)) - F(w - vec3(e, 0, 0)),
           F(w + vec3(0, e, 0)) - F(w - vec3(0, e, 0)),
           F(w + vec3(0, 0, e)) - F(w - vec3(0, 0, e)));
    return unit_vector(g);
}


// ==============================================================================================
// GEOMETRY
// ==============================================================================================

// Non-uniform scale plus shear, non-unit ray: hit t, world point, normal and UV are
// checked against an independent local-space quadratic and an implicit-surface normal.
static int geometry_sheared_ellipsoid() {
    auto matte = make_shared<lambertian>(color(0.4, 0.6, 0.8));
    auto local = make_shared<sphere>(point3(0, 0, 0), 1.0, matte);

    double A[3][3] = {{2.0, 0.5, 0.0},
                      {0.0, 1.0, 0.0},
                      {0.0, 0.0, 0.5}};
    point3 b0(0, 0, -5), b1(0, 0, -5);
    auto inst = affine_instance::make(local, A, b0, b1);
    TASK_CHECK(inst != nullptr);
    TASK_CHECK(inst->is_valid());

    double inv[3][3];
    test_inverse(A, inv);

    point3 origin(1.3, -0.7, 1.1);
    vec3 direction = 2.3 * vec3(-0.31, 0.17, -1.4);  // deliberately not unit length

    for (int k = 0; k < 3; k++) {
        double time = k * 0.5;
        vec3 b = test_offset(b0, b1, time);
        point3 local_origin = test_mul(inv, origin - b);
        vec3 local_dir = test_mul(inv, direction);

        double t_ref;
        point3 p_local;
        TASK_CHECK(local_sphere_hit(local_origin, local_dir, point3(0, 0, 0), 1.0,
                                    t_ref, p_local));

        ray r(origin, direction, time);
        hit_record rec;
        TASK_CHECK(inst->hit(r, interval(0.001, infinity), rec));
        TASK_CHECK(close(rec.t, t_ref, 1e-8));
        TASK_CHECK(close_vec(rec.p, r.at(rec.t), 1e-10));
        TASK_CHECK(close_vec(rec.p, test_mul(A, p_local) + b, 1e-8));

        vec3 n_local = unit_vector(p_local);
        vec3 n_ref = numeric_ellipsoid_normal(inv, b, rec.p);
        vec3 n_analytic = unit_vector(test_mul_t(inv, n_local));
        TASK_CHECK(close_vec(n_ref, n_analytic, 1e-5));
        TASK_CHECK(rec.front_face);
        TASK_CHECK(close_vec(rec.normal, n_ref, 1e-5));

        double u_ref, v_ref;
        sphere_uv_ref(n_local, u_ref, v_ref);
        TASK_CHECK(close(rec.u, u_ref, 1e-10));
        TASK_CHECK(close(rec.v, v_ref, 1e-10));
        TASK_CHECK(rec.mat == matte);
    }
    return 0;
}

// Negative determinant mirrors the sphere; inside/outside must not flip.
static int geometry_mirror_orientation() {
    auto matte = make_shared<lambertian>(color(0.8, 0.2, 0.2));
    auto local = make_shared<sphere>(point3(0, 0, 0), 1.0, matte);

    double A[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}};
    TASK_CHECK(test_det(A) < 0);
    point3 b(0, 0, 0);
    auto inst = affine_instance::make(local, A, b, b);
    TASK_CHECK(inst != nullptr);

    // Outside ray: front face on the near side.
    ray outside(point3(0, 0, -4), vec3(0, 0, 1), 0.37);
    hit_record rec;
    TASK_CHECK(inst->hit(outside, interval(0.001, infinity), rec));
    TASK_CHECK(rec.front_face);
    TASK_CHECK(close(rec.p.z(), -1.0, 1e-9));
    TASK_CHECK(close(rec.normal.z(), -1.0, 1e-6));

    double inv[3][3];
    test_inverse(A, inv);
    TASK_CHECK(close_vec(rec.normal,
                numeric_ellipsoid_normal(inv, b, rec.p), 1e-5));

    // Ray starting inside the ellipsoid: back face, mirroring does not swap sides.
    point3 inside_world = test_mul(A, point3(0.3, 0.1, 0.2));
    ray inside(inside_world, vec3(0.4, -0.2, 0.9), 0.8);
    TASK_CHECK(inst->hit(inside, interval(0.001, infinity), rec));
    TASK_CHECK(!rec.front_face);
    // Back-face normal points inward, opposite the exiting ray direction.
    TASK_CHECK(dot(rec.normal, inside.direction()) < 0);
    return 0;
}

// affine_instance nested inside translate and rotate_y: times survive, transforms do
// not get applied twice, and the list/BVH agree with the direct hit.
static int geometry_nested_transforms() {
    auto matte = make_shared<lambertian>(color(0.1, 0.9, 0.2));
    auto moving = make_shared<sphere>(point3(0, 0, 0), point3(0.6, 0.0, 0.3),
                                      1.0, matte);

    double A[3][3] = {{0.5, 0.0, 0.0},
                      {0.0, 0.8, 0.1},
                      {0.0, 0.0, 0.6}};
    point3 b0(0, 0, -1), b1(0.4, 0, -0.6);
    auto inst = affine_instance::make(moving, A, b0, b1);
    TASK_CHECK(inst != nullptr);
    shared_ptr<hittable> wrapped = make_shared<translate>(inst, vec3(1.2, 0, 0));
    wrapped = make_shared<rotate_y>(wrapped, 35);

    double invA[3][3];
    test_inverse(A, invA);
    double ang = degrees_to_radians(35);
    double Ry[3][3] = {{std::cos(ang), 0, std::sin(ang)},
                       {0, 1, 0},
                       {-std::sin(ang), 0, std::cos(ang)}};
    double Ry_inv[3][3];
    test_inverse(Ry, Ry_inv);
    vec3 translate_off(1.2, 0, 0);

    hittable_list world;
    world.add(wrapped);
    auto bvh = make_shared<bvh_node>(hittable_list(world));

    std::srand(1234);
    for (int trial = 0; trial < 40; trial++) {
        double time = random_double();
        point3 origin(random_double(-3, 3), random_double(-2, 2),
                      random_double(2, 4));
        vec3 direction(random_double(-0.6, 0.6), random_double(-0.6, 0.6),
                       random_double(-1.6, -0.8));

        // Independent reference: world = Ry * (translate(A p + b(t))).
        vec3 b = test_offset(b0, b1, time);
        point3 ref_origin = test_mul(invA, test_mul(Ry_inv, origin)
                                     - translate_off - b);
        vec3 ref_dir = test_mul(invA, test_mul(Ry_inv, direction));
        point3 sphere_center = (1.0 - time) * point3(0, 0, 0)
                             + time * point3(0.6, 0, 0.3);
        double t_ref;
        point3 p_local;
        bool ref_hit = local_sphere_hit(ref_origin, ref_dir, sphere_center, 1.0,
                                        t_ref, p_local);

        ray r(origin, direction, time);
        hit_record rec, list_rec, bvh_rec;
        bool got = wrapped->hit(r, interval(0.001, infinity), rec);
        bool got_list = world.hit(r, interval(0.001, infinity), list_rec);
        bool got_bvh = bvh->hit(r, interval(0.001, infinity), bvh_rec);
        TASK_CHECK(got == ref_hit);
        TASK_CHECK(got_list == got);
        TASK_CHECK(got_bvh == got);
        if (got) {
            TASK_CHECK(close(rec.t, t_ref, 1e-7));
            TASK_CHECK(close_vec(rec.p, r.at(rec.t), 1e-9));
            TASK_CHECK(close(list_rec.t, rec.t, 1e-10));
            TASK_CHECK(close(bvh_rec.t, rec.t, 1e-9));
            TASK_CHECK(rec.mat == matte);
        }
    }
    return 0;
}

// Bounding boxes cover the whole shutter; BVH misses no hits across the time interval.
static int geometry_bvh_shutter_coverage() {
    auto matte = make_shared<lambertian>(color(0.5, 0.5, 0.5));
    hittable_list list;
    std::vector<shared_ptr<affine_instance>> insts;
    std::srand(99);
    for (int k = 0; k < 5; k++) {
        auto local = make_shared<sphere>(point3(0, 0, 0), 0.8, matte);
        double A[3][3] = {{random_double(0.5, 1.4), random_double(-0.3, 0.3), 0.0},
                          {0.0, random_double(0.5, 1.3), random_double(-0.2, 0.2)},
                          {0.0, 0.0, random_double(0.5, 1.2)}};
        point3 b0(random_double(-2, 2), random_double(-1, 1), random_double(-3, -2));
        point3 b1(b0 + vec3(random_double(-1.5, 1.5), random_double(-1, 1),
                            random_double(-0.5, 0.5)));
        auto inst = affine_instance::make(local, A, b0, b1);
        TASK_CHECK(inst != nullptr);
        insts.push_back(inst);
        list.add(inst);

        // Every transformed corner at t = 0, 0.5, 1 must lie inside the instance box.
        aabb box = inst->bounding_box();
        for (int ti = 0; ti < 3; ti++) {
            double t = ti * 0.5;
            vec3 b = test_offset(b0, b1, t);
            for (int c = 0; c < 8; c++) {
                point3 corner((c & 1) ? 0.8 : -0.8,
                              (c & 2) ? 0.8 : -0.8,
                              (c & 4) ? 0.8 : -0.8);
                point3 w = test_mul(A, corner) + b;
                TASK_CHECK(box.x.min - 1e-9 <= w.x() && w.x() <= box.x.max + 1e-9);
                TASK_CHECK(box.y.min - 1e-9 <= w.y() && w.y() <= box.y.max + 1e-9);
                TASK_CHECK(box.z.min - 1e-9 <= w.z() && w.z() <= box.z.max + 1e-9);
            }
        }
    }

    auto bvh = make_shared<bvh_node>(hittable_list(list));
    TASK_CHECK(bvh->bounding_box().x.min <= list.bounding_box().x.min + 1e-9);

    for (int trial = 0; trial < 300; trial++) {
        double time = random_double();
        ray r(point3(random_double(-4, 4), random_double(-3, 3),
                     random_double(1, 3)),
              vec3(random_double(-1, 1), random_double(-1, 1),
                   random_double(-1.5, -0.4)),
              time);
        hit_record a, b;
        bool ha = list.hit(r, interval(0.001, infinity), a);
        bool hb = bvh->hit(r, interval(0.001, infinity), b);
        TASK_CHECK(ha == hb);
        if (ha) {
            TASK_CHECK(close(a.t, b.t, 1e-8));
            TASK_CHECK(close_vec(a.p, b.p, 1e-7));
        }
    }
    return 0;
}

// One source object shared by two instances stays stationary itself.
static int geometry_shared_source() {
    auto matte = make_shared<lambertian>(color(0.3, 0.3, 0.7));
    auto source = make_shared<sphere>(point3(0, 0, 0), 1.0, matte);
    aabb original_box = source->bounding_box();

    double A1[3][3] = {{2, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double A2[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 2}};
    point3 b(0, 0, -4);
    auto i1 = affine_instance::make(source, A1, b, b);
    auto i2 = affine_instance::make(source, A2, b + vec3(3, 0, 0),
                                    b + vec3(3, 0, 0));
    TASK_CHECK(i1 != nullptr && i2 != nullptr);

    ray r(point3(0, 0, 1), vec3(0, 0, -1), 0.5);
    hit_record r1, r2, rs;
    TASK_CHECK(i1->hit(r, interval(0.001, infinity), r1));
    TASK_CHECK(close(r1.p.z(), -3.0, 1e-9));   // scaled x-axis, z radius still 1

    ray r2ray(point3(3, 0, 1), vec3(0, 0, -1), 0.5);
    TASK_CHECK(i2->hit(r2ray, interval(0.001, infinity), r2));
    TASK_CHECK(close(r2.p.z(), -2.0, 1e-9));   // z scaled by 2 around (3,0,-4)

    TASK_CHECK(source->hit(ray(point3(0, 0, 2), vec3(0, 0, -1)),
                           interval(0.001, infinity), rs));
    TASK_CHECK(close(rs.t, 1.0, 1e-9));
    TASK_CHECK(close(source->bounding_box().x.min, original_box.x.min, 1e-12));
    return 0;
}

// Singular, non-finite matrices and endpoints are rejected.
static int geometry_reject_bad_input() {
    auto local = make_shared<sphere>(point3(0, 0, 0), 1.0,
                                     make_shared<lambertian>(color(0, 0, 0)));

    double singular[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    TASK_CHECK(std::fabs(test_det(singular)) < 1e-9);
    TASK_CHECK(affine_instance::make(local, singular, point3(0,0,0), point3(0,0,0))
               == nullptr);

    double nan_mat[3][3] = {{1, 0, 0}, {0, std::nan(""), 0}, {0, 0, 1}};
    TASK_CHECK(affine_instance::make(local, nan_mat, point3(0,0,0), point3(0,0,0))
               == nullptr);

    double good[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    TASK_CHECK(affine_instance::make(local, good, point3(0, 0, std::nan("")),
                                     point3(0,0,0)) == nullptr);
    TASK_CHECK(affine_instance::make(local, good, point3(0,0,0),
                                     point3(std::nan(""), 0, 0)) == nullptr);

    affine_instance direct(local, singular, point3(0,0,0), point3(0,0,0));
    TASK_CHECK(!direct.is_valid());
    hit_record rec;
    TASK_CHECK(!direct.hit(ray(point3(0, 0, 1), vec3(0, 0, -1)),
                           interval(0.001, infinity), rec));
    return 0;
}


// ==============================================================================================
// DENSITY
// ==============================================================================================

// Solid angle of the spherical triangle (a, b, c) on the unit sphere (Van Oosterom).
static double spherical_triangle_area(const vec3& a, const vec3& b, const vec3& c) {
    double num = std::fabs(dot(a, cross(b, c)));
    double den = 1.0 + dot(a, b) + dot(b, c) + dot(c, a);
    return 2.0 * std::atan(num / den);
}

// Independent small-cap area difference for the direction mapping d_w = A d_l.
static double measured_jacobian(const double A[3][3], const vec3& omega_local,
                                double eps, int steps) {
    onb frame(omega_local);
    vec3 center = unit_vector(test_mul(A, omega_local));
    double mapped_area = 0.0;
    vec3 prev_edge;
    for (int k = 0; k <= steps; k++) {
        double phi = 2 * pi * k / steps;
        vec3 tangent = eps * (std::cos(phi) * frame.u() + std::sin(phi) * frame.v());
        vec3 wl = unit_vector(omega_local + tangent);
        vec3 edge = unit_vector(test_mul(A, wl));
        if (k > 0)
            mapped_area += spherical_triangle_area(center, prev_edge, edge);
        prev_edge = edge;
    }
    double local_cap_area = pi * eps * eps;
    return mapped_area / local_cap_area;
}

static int density_jacobian_cap() {
    auto matte = make_shared<lambertian>(color(0, 0, 0));
    auto local_sphere = make_shared<sphere>(point3(0, 0, 0), 1.0, matte);

    struct Case { double A[3][3]; const char* tag; };
    double shear[3][3] = {{2.0, 0.6, 0.1},
                          {0.0, 1.0, 0.2},
                          {0.0, 0.0, 0.5}};
    double mirror[3][3] = {{1.5, 0.3, 0.0},
                           {0.0, -1.0, 0.0},
                           {0.4, 0.0, 0.7}};
    const double* mats[2] = {&shear[0][0], &mirror[0][0]};

    for (int mi = 0; mi < 2; mi++) {
        double A[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                A[i][j] = mats[mi][i * 3 + j];
        double detA = test_det(A);
        double inv[3][3];
        test_inverse(A, inv);

        point3 b0(0, 0, -6), b1(0.5, 0, -5.5);
        auto inst = affine_instance::make(local_sphere, A, b0, b1);
        TASK_CHECK(inst != nullptr);

        std::srand(77 + mi);
        for (int sample = 0; sample < 6; sample++) {
            double time = random_double();
            vec3 b = test_offset(b0, b1, time);
            // World origin chosen so its local image is well outside the unit sphere.
            point3 origin = b + test_mul(A, point3(0.2, -0.3, 4.0));
            point3 local_origin = test_mul(inv, origin - b);
            TASK_CHECK((local_origin - point3(0, 0, 0)).length() > 1.5);

            vec3 target_local = unit_vector(vec3(random_double(-0.12, 0.12),
                                                 random_double(-0.12, 0.12), -1.0));
            vec3 world_dir = test_mul(A, target_local);

            // Independent Jacobian from finite cap areas.
            double j_measured = measured_jacobian(A, target_local, 0.01, 4000);
            double j_formula = std::fabs(detA)
                             / std::pow(test_mul(A, target_local).length(), 3);
            TASK_CHECK(close(j_measured, j_formula, 1.5e-3));

            double p_local = local_sphere->pdf_value(local_origin, target_local);
            double p_world = inst->pdf_value(origin, world_dir, time);
            TASK_CHECK(p_local > 0 && p_world > 0);
            TASK_CHECK(close(p_world * j_measured, p_local, 1.5e-3));

            // Positive rescaling of the query direction leaves the density unchanged.
            TASK_CHECK(close(inst->pdf_value(origin, 3.7 * world_dir, time),
                             p_world, 1e-12));
            // Negative determinant still yields a non-negative density.
            TASK_CHECK(p_world >= 0.0);
        }
    }
    return 0;
}

// Fixed-seed direction frequencies vs. the claimed density, plus full-sphere
// numerical normalization for both an ellipsoid and a sheared quad.
static int density_normalization() {
    auto matte = make_shared<lambertian>(color(0, 0, 0));
    auto local_sphere = make_shared<sphere>(point3(0, 0, 0), 1.0, matte);
    double A[3][3] = {{1.7, 0.4, 0.0},
                      {0.0, 0.8, 0.3},
                      {0.2, 0.0, 0.6}};
    point3 b0(0, 0, -7), b1(1.0, 0.5, -6.5);
    auto inst = affine_instance::make(local_sphere, A, b0, b1);
    TASK_CHECK(inst != nullptr);
    double inv[3][3];
    test_inverse(A, inv);

    // The illuminated cone is narrow, so the histogram bins must be small enough for
    // several of them to fall inside the cone.
    const int n_theta = 36, n_phi = 72;
    // The illuminated cone is narrow, so the normalization integral needs a much
    // finer grid than the frequency histogram.
    const int f_theta = 96, f_phi = 192;
    auto bin_center = [&](int it, int ip, int nt, int np) {
        double theta = pi * (it + 0.5) / nt;
        double phi = 2 * pi * (ip + 0.5) / np;
        return vec3(std::sin(theta) * std::cos(phi),
                    std::cos(theta),
                    std::sin(theta) * std::sin(phi));
    };
    auto bin_solid_angle = [&](int it, int nt, int np) {
        double cos_lo = std::cos(pi * it / nt);
        double cos_hi = std::cos(pi * (it + 1) / nt);
        return (cos_lo - cos_hi) * (2 * pi / np);
    };

    double time = 0.42;
    vec3 b = test_offset(b0, b1, time);
    point3 origin = b + test_mul(A, point3(0.1, 0.2, 5.0));

    // Numerical integration over the whole world direction sphere on the fine grid.
    double integrated = 0.0;
    for (int it = 0; it < f_theta; it++) {
        for (int ip = 0; ip < f_phi; ip++) {
            integrated += inst->pdf_value(origin,
                                          bin_center(it, ip, f_theta, f_phi), time)
                        * bin_solid_angle(it, f_theta, f_phi);
        }
    }
    TASK_CHECK(close(integrated, 1.0, 3e-3));

    // Coarse-bin masses used by the frequency histogram. Each bin mass is integrated
    // on its own sub-grid, so cone-edge bins get accurate, nonzero expectations.
    const int sub = 6;
    std::vector<double> bin_mass(n_theta * n_phi, 0.0);
    for (int it = 0; it < n_theta; it++) {
        for (int ip = 0; ip < n_phi; ip++) {
            double mass = 0.0;
            for (int a = 0; a < sub; a++) {
                for (int c = 0; c < sub; c++) {
                    double theta = pi * (it + (a + 0.5) / sub) / n_theta;
                    double phi = 2 * pi * (ip + (c + 0.5) / sub) / n_phi;
                    vec3 dir(std::sin(theta) * std::cos(phi),
                             std::cos(theta),
                             std::sin(theta) * std::sin(phi));
                    double cell_sa = (std::cos(pi * it / n_theta)
                                      - std::cos(pi * (it + 1) / n_theta))
                                   * (2 * pi / n_phi) / (sub * sub);
                    mass += inst->pdf_value(origin, dir, time) * cell_sa;
                }
            }
            bin_mass[it * n_phi + ip] = mass;
        }
    }

    // Fixed-seed empirical frequencies.
    const int N = 12000;
    std::vector<int> counts(n_theta * n_phi, 0);
    std::srand(20240601);
    for (int k = 0; k < N; k++) {
        vec3 d = inst->random(origin, time);
        TASK_CHECK(std::isfinite(d.x()) && std::isfinite(d.y())
                   && std::isfinite(d.z()));
        vec3 u = unit_vector(d);
        double theta = std::acos(std::fmax(-1.0, std::fmin(1.0, u.y())));
        double phi = std::atan2(u.z(), u.x());
        if (phi < 0) phi += 2 * pi;
        int it = std::fmin(n_theta - 1, (int)(theta / pi * n_theta));
        int ip = std::fmin(n_phi - 1, (int)(phi / (2 * pi) * n_phi));
        counts[it * n_phi + ip]++;
    }
    int occupied = 0;
    double tv_distance = 0.0;
    for (int idx = 0; idx < n_theta * n_phi; idx++) {
        double expected = bin_mass[idx];
        double observed = (double)counts[idx] / N;
        tv_distance += std::fabs(observed - expected);
        if (expected > 0.0 || observed > 0.0) occupied++;
    }
    tv_distance *= 0.5;  // total variation distance between the two bin distributions
    std::cout << "histogram occupied_bins=" << occupied
              << " samples=" << N << " tv_distance=" << tv_distance << "\n";
    TASK_CHECK(occupied > 20);
    TASK_CHECK(tv_distance < 0.08);

    // Same normalization exercise for a sheared quad; origin stays off its plane.
    auto local_quad = make_shared<quad>(point3(-1, -1, 0), vec3(2, 0, 0),
                                        vec3(0, 2, 0), matte);
    double Aq[3][3] = {{1.0, 0.5, 0.0},
                       {0.0, 1.0, 0.0},
                       {0.0, 0.0, 1.0}};
    point3 qb(0, 0, -5);
    auto qinst = affine_instance::make(local_quad, Aq, qb, qb);
    TASK_CHECK(qinst != nullptr);
    double invq[3][3];
    test_inverse(Aq, invq);
    point3 q_origin = qb + test_mul(Aq, point3(0.2, 0.1, 4.0));
    TASK_CHECK(std::fabs((test_mul(invq, q_origin - qb)).z()) > 1.0);

    // The quad density has a sharp support boundary, so instead of another grid we
    // integrate over uniform world-sphere directions (fully independent of generate).
    const int M = 200000;
    std::srand(909090);
    double mc_sum = 0.0, mc_sq = 0.0;
    for (int k = 0; k < M; k++) {
        double value = 4 * pi * qinst->pdf_value(q_origin, random_unit_vector(), 0.0);
        mc_sum += value;
        mc_sq += value * value;
    }
    double mc_mean = mc_sum / M;
    double mc_var = mc_sq / M - mc_mean * mc_mean;
    double mc_se = std::sqrt(std::fmax(0.0, mc_var) / M);
    std::cout << "quad normalization estimate=" << mc_mean
              << " standard_error=" << mc_se << "\n";
    TASK_CHECK(std::fabs(mc_mean - 1.0) <= 5.0 * mc_se + 0.005);
    return 0;
}

// Moving sphere wrapped in a moving instance, a two-instance light list, and a legacy
// custom hittable that only implements the old time-less virtual functions.
class legacy_custom_light : public hittable {
  public:
    legacy_custom_light(const point3& c, double r, shared_ptr<material> mat)
      : impl(c, r, mat) {}

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        return impl.hit(r, ray_t, rec);
    }
    aabb bounding_box() const override { return impl.bounding_box(); }
    // Deliberately override only the old entry points.
    double pdf_value(const point3& origin, const vec3& direction) const override {
        return impl.pdf_value(origin, direction);
    }
    vec3 random(const point3& origin) const override {
        return impl.random(origin);
    }

  private:
    sphere impl;
};

static int density_motion_list_legacy() {
    auto matte = make_shared<lambertian>(color(0, 0, 0));

    // Moving local sphere plus a moving instance on top of it.
    auto moving = make_shared<sphere>(point3(0, 0, 0), point3(0.5, 0, 0),
                                      1.0, matte);
    double A[3][3] = {{1.3, 0.2, 0.0}, {0.0, 0.9, 0.0}, {0.0, 0.0, 1.1}};
    point3 b0(0, 0, -6), b1(1.2, 0, -6);
    auto inst1 = affine_instance::make(moving, A, b0, b1);
    TASK_CHECK(inst1 != nullptr);

    // Second instance shares the very same source object.
    double A2[3][3] = {{0.7, 0.0, 0.0}, {0.0, 1.2, 0.3}, {0.0, 0.0, 0.9}};
    auto inst2 = affine_instance::make(moving, A2, b0 + vec3(-3, 1, 0),
                                       b1 + vec3(-3, 1, 0));
    TASK_CHECK(inst2 != nullptr);

    auto lights = make_shared<hittable_list>();
    lights->add(inst1);
    lights->add(inst2);

    double inv[3][3];
    test_inverse(A, inv);

    for (int k = 0; k < 5; k++) {
        double time = k * 0.25;
        vec3 b = test_offset(b0, b1, time);
        point3 origin = b + test_mul(A, point3(0, 0, 4.5));

        // Independent reference: local object density times inverse Jacobian.
        point3 local_origin = test_mul(inv, origin - b);
        vec3 local_target = unit_vector(point3(0.1, -0.1, -1.0));
        vec3 world_dir = test_mul(A, local_target);
        double p_local = moving->pdf_value(local_origin, local_target, time);
        double jac = std::fabs(test_det(A))
                   / std::pow(test_mul(A, local_target).length(), 3);
        double p_ref = p_local / jac;
        TASK_CHECK(close(inst1->pdf_value(origin, world_dir, time), p_ref, 1e-10));

        // The list is the uniform average of the two instance densities.
        double p_list = lights->pdf_value(origin, world_dir, time);
        double p_second = inst2->pdf_value(origin, world_dir, time);
        TASK_CHECK(close(p_list, 0.5 * p_ref + 0.5 * p_second, 1e-10));
    }

    // Legacy object: time-aware calls fall back to the old time-less implementation.
    auto legacy = make_shared<legacy_custom_light>(point3(0, 0, 0), 1.0, matte);
    double Ai[3][3] = {{1.4, 0.0, 0.0}, {0.0, 0.8, 0.0}, {0.0, 0.0, 1.2}};
    point3 lb(0, 0, -5);
    auto linst = affine_instance::make(legacy, Ai, lb, lb);
    TASK_CHECK(linst != nullptr);
    double linv[3][3];
    test_inverse(Ai, linv);
    point3 l_origin = lb + test_mul(Ai, point3(0, 0, 3.5));
    point3 ll_origin = test_mul(linv, l_origin - lb);
    vec3 l_world = test_mul(Ai, vec3(0.1, 0.1, -1));
    vec3 l_local = test_mul(linv, l_world);
    double legacy_p = legacy->pdf_value(ll_origin, l_local);
    // Full solid-angle Jacobian, including the (non-unit) local direction length.
    double ljac = std::fabs(test_det(Ai))
                * std::pow(l_local.length(), 3)
                / std::pow(test_mul(Ai, l_local).length(), 3);
    TASK_CHECK(close(linst->pdf_value(l_origin, l_world, 0.5),
                     legacy_p / ljac, 1e-10));
    TASK_CHECK(close(linst->pdf_value(l_origin, l_world, 0.0),
                     linst->pdf_value(l_origin, l_world, 0.5), 1e-12));

    // The old two-argument hittable_pdf keeps its original dispatch.
    hittable_pdf old_pdf(*legacy, ll_origin);
    TASK_CHECK(close(old_pdf.value(l_local), legacy_p, 1e-12));
    std::srand(4242);
    vec3 g1 = old_pdf.generate();
    std::srand(4242);
    vec3 g2 = legacy->random(ll_origin);
    TASK_CHECK(close_vec(g1, g2, 1e-15));

    // The new constructor carries the time through the same object.
    hittable_pdf timed_pdf(*linst, l_origin, 0.7);
    TASK_CHECK(std::isfinite(timed_pdf.value(l_world)));
    return 0;
}

// translate and rotate_y must forward the same shutter time into the local object's
// sampling entry points, with the point/direction mapped through the same transform as
// ray intersection.
static int density_wrapper_time_forwarding() {
    auto matte = make_shared<lambertian>(color(0, 0, 0));
    auto moving = make_shared<sphere>(point3(0, 0, 0), point3(0.7, 0.0, 0.2),
                                      1.0, matte);
    vec3 off(1.1, -0.4, 0.6);
    auto moved = make_shared<translate>(moving, off);
    auto rotated = make_shared<rotate_y>(moving, 90);

    point3 origin(0.3, 0.2, 4.0);
    vec3 direction(0.15, -0.1, -1.0);

    for (int k = 0; k < 4; k++) {
        double t = k / 3.0;

        // translate: density uses origin - offset and the identical time.
        TASK_CHECK(close(moved->pdf_value(origin, direction, t),
                         moving->pdf_value(origin - off, direction, t), 1e-12));
        // The proposal is the local proposal at the shifted origin with the same time.
        std::srand(555 + k);
        vec3 local_proposal = moving->random(origin - off, t);
        std::srand(555 + k);
        TASK_CHECK(close_vec(moved->random(origin, t), local_proposal, 1e-12));

        // rotate_y(90 deg): inverse rotation on the query, rotation on the proposal.
        double c = std::cos(degrees_to_radians(90));
        double s = std::sin(degrees_to_radians(90));
        auto rot = [&](const vec3& v) {
            return vec3(c*v.x() + s*v.z(), v.y(), -s*v.x() + c*v.z());
        };
        auto unrot = [&](const vec3& v) {
            return vec3(c*v.x() - s*v.z(), v.y(), s*v.x() + c*v.z());
        };
        TASK_CHECK(close(rotated->pdf_value(origin, direction, t),
                         moving->pdf_value(unrot(origin), unrot(direction), t), 1e-12));
        std::srand(777 + k);
        vec3 rotated_local = moving->random(unrot(origin), t);
        std::srand(777 + k);
        TASK_CHECK(close_vec(rotated->random(origin, t), rot(rotated_local), 1e-12));

        // Time really reaches the moving sphere: different times give different values.
        if (k > 0) {
            TASK_CHECK(!close(moved->pdf_value(origin, direction, t),
                              moved->pdf_value(origin, direction, 0.0), 1e-9));
        }
    }

    // Old time-less entries keep their pre-existing behavior: translate/rotate_y never
    // overrode them, so the base default (zero density) is unchanged.
    TASK_CHECK(moved->pdf_value(origin, direction) == 0.0);
    TASK_CHECK(rotated->pdf_value(origin, direction) == 0.0);

    // Nested affine inside translate: one time flows through both layers unchanged.
    double A[3][3] = {{1.2, 0.3, 0.0}, {0.0, 0.9, 0.0}, {0.0, 0.0, 1.1}};
    auto inst = affine_instance::make(moving, A, point3(0, 0, -2),
                                      point3(0.5, 0, -2));
    TASK_CHECK(inst != nullptr);
    auto nested = make_shared<translate>(inst, off);
    double t = 0.63;
    TASK_CHECK(close(nested->pdf_value(origin, direction, t),
                     inst->pdf_value(origin - off, direction, t), 1e-12));
    std::srand(909);
    vec3 nested_inner = inst->random(origin - off, t);
    std::srand(909);
    TASK_CHECK(close_vec(nested->random(origin, t), nested_inner, 1e-12));
    return 0;
}

// Zero / non-finite queries return zero density; illegal times are rejected loudly.
static int density_query_guards() {
    auto matte = make_shared<lambertian>(color(0, 0, 0));
    auto local = make_shared<sphere>(point3(0, 0, 0), 1.0, matte);
    double A[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}};  // negative determinant
    point3 b(0, 0, -5);
    auto inst = affine_instance::make(local, A, b, b);
    TASK_CHECK(inst != nullptr);
    point3 origin(0, 0, 0);

    TASK_CHECK(inst->pdf_value(origin, vec3(0, 0, 0), 0.5) == 0.0);
    TASK_CHECK(inst->pdf_value(origin, vec3(std::nan(""), 0, -1), 0.5) == 0.0);
    TASK_CHECK(inst->pdf_value(origin, vec3(0, std::nan(""), -1), 0.5) == 0.0);
    TASK_CHECK(inst->pdf_value(origin, vec3(0, 0, std::nan("")), 0.5) == 0.0);

    bool threw = false;
    try { inst->pdf_value(origin, vec3(0, 0, -1), 1.01); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);

    threw = false;
    try { inst->pdf_value(origin, vec3(0, 0, -1), -0.01); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);

    threw = false;
    try { inst->random(origin, std::nan("")); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);

    // Endpoint times are legal.
    TASK_CHECK(inst->pdf_value(origin, vec3(0, 0, -1), 0.0) >= 0.0);
    TASK_CHECK(inst->pdf_value(origin, vec3(0, 0, -1), 1.0) >= 0.0);

    // A world origin whose local image lies inside the unit sphere violates the
    // local sphere's sampling precondition; the wrapper must return zero, not NaN.
    point3 inside_world = b + test_mul(A, point3(0.2, 0.1, 0.3));
    double guarded = inst->pdf_value(inside_world, vec3(0, 0, -1), 0.5);
    TASK_CHECK(std::isfinite(guarded) && guarded == 0.0);
    return 0;
}


// ==============================================================================================
// TRANSPORT
// ==============================================================================================

struct call_record { char kind; double time; };

// Forwards every call to an inner hittable and records the time it carried.
class recording_hittable : public hittable {
  public:
    recording_hittable(shared_ptr<hittable> inner, std::vector<call_record>* log)
      : inner(inner), log(log) {}

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        log->push_back({'h', r.time()});
        return inner->hit(r, ray_t, rec);
    }
    aabb bounding_box() const override { return inner->bounding_box(); }

    double pdf_value(const point3& o, const vec3& d) const override {
        log->push_back({'p', 0.0});
        return inner->pdf_value(o, d);
    }
    vec3 random(const point3& o) const override {
        log->push_back({'g', 0.0});
        return inner->random(o);
    }
    double pdf_value(const point3& o, const vec3& d, double t) const override {
        log->push_back({'p', t});
        return inner->pdf_value(o, d, t);
    }
    vec3 random(const point3& o, double t) const override {
        log->push_back({'g', t});
        return inner->random(o, t);
    }

  private:
    shared_ptr<hittable> inner;
    std::vector<call_record>* log;
};

struct transport_scene {
    shared_ptr<lambertian> floor_mat;
    shared_ptr<diffuse_light> light_mat;
    shared_ptr<affine_instance> light_inst;
    shared_ptr<quad> floor;
    shared_ptr<hittable> occluder;
};

static transport_scene make_scene(bool shear, bool occluded) {
    transport_scene s;
    s.floor_mat = make_shared<lambertian>(color(0.8, 0.8, 0.8));
    s.light_mat = make_shared<diffuse_light>(color(5, 5, 5));

    auto local_quad = make_shared<quad>(point3(-0.75, 0.75, 0), vec3(1.5, 0, 0),
                                        vec3(0, -1.5, 0), s.light_mat);
    double A[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
    if (shear) {
        A[0][1] = 0.25;
        A[1][0] = 0.10;
    }
    s.light_inst = affine_instance::make(local_quad, A,
                                         point3(-0.8, 0, -2),
                                         point3(0.8, 0, -2));
    s.floor = make_shared<quad>(point3(-10, -10, -4), vec3(20, 0, 0),
                                vec3(0, 20, 0), s.floor_mat);
    if (occluded)
        s.occluder = make_shared<sphere>(point3(0, 0, -3), 0.45, s.floor_mat);
    return s;
}

// One camera ray: hit, proposal and pdf times must all be that ray's shutter time.
static int transport_single_ray_consistency() {
    transport_scene s = make_scene(true, false);
    std::vector<call_record> log;
    auto recorded_light = make_shared<recording_hittable>(s.light_inst, &log);

    hittable_list world, lights;
    world.add(s.floor);
    world.add(recorded_light);
    lights.add(recorded_light);

    camera cam;
    cam.image_width = 1;
    cam.aspect_ratio = 1;
    cam.samples_per_pixel = 1;
    cam.max_depth = 5;
    cam.background = color(0, 0, 0);
    cam.enable_time_lights = true;

    std::ostringstream image;
    auto* old_buf = std::cout.rdbuf(image.rdbuf());
    cam.render(world, lights);
    std::cout.rdbuf(old_buf);

    TASK_CHECK(!log.empty());
    double ray_time = log.front().time;
    TASK_CHECK(ray_time >= 0.0 && ray_time <= 1.0);
    bool saw_hit = false, saw_pdf = false;
    for (size_t i = 0; i < log.size(); i++) {
        TASK_CHECK(close(log[i].time, ray_time, 1e-15));
        if (log[i].kind == 'h') saw_hit = true;
        if (log[i].kind == 'p') saw_pdf = true;
    }
    TASK_CHECK(saw_hit && saw_pdf);

    // With the flag off, light proposals carry time 0 even though hits keep ray time.
    std::vector<call_record> old_log;
    auto old_recorded = make_shared<recording_hittable>(s.light_inst, &old_log);
    hittable_list old_world, old_lights;
    old_world.add(s.floor);
    old_world.add(old_recorded);
    old_lights.add(old_recorded);
    camera cam2;
    cam2.image_width = 1;
    cam2.aspect_ratio = 1;
    cam2.samples_per_pixel = 1;
    cam2.max_depth = 4;
    cam2.background = color(0, 0, 0);
    TASK_CHECK(!cam2.enable_time_lights);

    std::ostringstream image2;
    old_buf = std::cout.rdbuf(image2.rdbuf());
    cam2.render(old_world, old_lights);
    std::cout.rdbuf(old_buf);

    bool light_call_seen = false;
    for (size_t i = 0; i < old_log.size(); i++) {
        if (old_log[i].kind == 'h') continue;
        TASK_CHECK(old_log[i].time == 0.0);
        light_call_seen = true;
    }
    TASK_CHECK(light_call_seen);
    return 0;
}

// Many rays: every proposal/pdf time must match a real intersection ray's time.
static int transport_multi_ray_times() {
    transport_scene s = make_scene(true, false);
    std::vector<call_record> log;
    auto recorded_light = make_shared<recording_hittable>(s.light_inst, &log);

    hittable_list world, lights;
    world.add(s.floor);
    world.add(recorded_light);
    lights.add(recorded_light);

    camera cam;
    cam.image_width = 8;
    cam.aspect_ratio = 1;
    cam.samples_per_pixel = 4;
    cam.max_depth = 5;
    cam.background = color(0, 0, 0);
    cam.enable_time_lights = true;

    std::ostringstream image;
    auto* old_buf = std::cout.rdbuf(image.rdbuf());
    cam.render(world, lights);
    std::cout.rdbuf(old_buf);

    std::vector<double> hit_times;
    size_t proposals = 0;
    for (size_t i = 0; i < log.size(); i++) {
        if (log[i].kind == 'h') {
            bool known = false;
            for (size_t j = 0; j < hit_times.size(); j++)
                if (close(hit_times[j], log[i].time, 1e-15)) known = true;
            if (!known) hit_times.push_back(log[i].time);
        }
    }
    TASK_CHECK(hit_times.size() >= 48);  // 64 camera rays, near-zero collision odds
    for (size_t i = 0; i < log.size(); i++) {
        if (log[i].kind == 'h') continue;
        bool matched = false;
        for (size_t j = 0; j < hit_times.size(); j++)
            if (close(hit_times[j], log[i].time, 1e-15)) matched = true;
        TASK_CHECK(matched);
        if (log[i].kind == 'g') proposals++;
    }
    TASK_CHECK(proposals > 10);  // the light branch of the mixture was really used
    return 0;
}

// Independent hemisphere grid integral for direct irradiance at a receiver point.
static double grid_direct_light(const point3& origin, const vec3& normal,
                                const hittable& world,
                                shared_ptr<material> light_mat, double time,
                                int n_cos = 36, int n_phi = 72) {
    onb frame(normal);
    double total = 0.0;
    double d_cos = 1.0 / n_cos, d_phi = 2 * pi / n_phi;
    for (int ic = 0; ic < n_cos; ic++) {
        double cos_theta = (ic + 0.5) * d_cos;
        double sin_theta = std::sqrt(std::fmax(0.0, 1.0 - cos_theta * cos_theta));
        for (int ip = 0; ip < n_phi; ip++) {
            double phi = (ip + 0.5) * d_phi;
            vec3 dir = frame.transform(vec3(sin_theta * std::cos(phi),
                                             sin_theta * std::sin(phi),
                                             cos_theta));
            hit_record rec;
            if (world.hit(ray(origin, dir, time), interval(0.001, infinity), rec)
                && rec.mat == light_mat) {
                total += 5.0 * (cos_theta / pi) * (d_cos * d_phi);
            }
        }
    }
    return total;
}

// Importance-sampled estimator using the same mixture denominator as the renderer.
static double importance_direct_light(const point3& origin, const vec3& normal,
                                      const hittable& world,
                                      const hittable& lights,
                                      shared_ptr<material> light_mat, double time,
                                      int samples) {
    hittable_pdf light_pdf(lights, origin, time);
    double sum = 0.0;
    for (int k = 0; k < samples; k++) {
        vec3 dir = light_pdf.generate();
        double density = light_pdf.value(dir);
        if (!(density > 0.0)) continue;
        hit_record rec;
        double cosine = dot(normal, unit_vector(dir));
        if (cosine <= 0.0) continue;
        bool visible = world.hit(ray(origin, dir, time),
                                 interval(0.001, infinity), rec)
                       && rec.mat == light_mat;
        if (visible)
            sum += 5.0 * (cosine / pi) / density;
    }
    return sum / samples;
}

// At fixed shutter times, the time-aware hittable_pdf matches an independent grid
// integral; occlusion and no-transform controls behave as required.
static int transport_fixed_time_integral() {
    const point3 receiver(0, 0, -3.999);
    const vec3 normal(0, 0, 1);

    transport_scene scene = make_scene(true, false);
    hittable_list world, lights;
    world.add(scene.floor);
    world.add(scene.light_inst);
    lights.add(scene.light_inst);

    transport_scene blocked = make_scene(true, true);
    hittable_list bworld, blights;
    bworld.add(blocked.floor);
    bworld.add(blocked.occluder);
    bworld.add(blocked.light_inst);
    blights.add(blocked.light_inst);

    std::srand(31337);
    double times[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
    for (int k = 0; k < 5; k++) {
        double t = times[k];
        double grid = grid_direct_light(receiver, normal, world, scene.light_mat, t);
        double iss = importance_direct_light(receiver, normal, world, lights,
                                             scene.light_mat, t, 3000);
        TASK_CHECK(grid > 0.05);
        TASK_CHECK(std::fabs(grid - iss) <= 0.08 * (1.0 + grid));

        double grid_blocked = grid_direct_light(receiver, normal, bworld,
                                                blocked.light_mat, t);
        double iss_blocked = importance_direct_light(receiver, normal, bworld,
                                                     blights, blocked.light_mat,
                                                     t, 3000);
        TASK_CHECK(grid_blocked < grid);
        TASK_CHECK(std::fabs(grid_blocked - iss_blocked)
                   <= 0.1 * (1.0 + grid_blocked));
    }

    // No-transform control: a stationary light gives the same answer at every time,
    // and the old and time-aware pdf entry points agree exactly.
    transport_scene stationary = make_scene(false, false);
    double I[3][3]; mat_identity(I);
    point3 mid(0, 0, -2);
    auto static_light = affine_instance::make(
        make_shared<quad>(point3(-0.75, 0.75, 0), vec3(1.5, 0, 0), vec3(0, -1.5, 0),
                          stationary.light_mat),
        I, mid, mid);
    TASK_CHECK(static_light != nullptr);
    hittable_list sworld, slights;
    sworld.add(stationary.floor);
    sworld.add(static_light);
    slights.add(static_light);

    double g0 = grid_direct_light(receiver, normal, sworld,
                                  stationary.light_mat, 0.1);
    double g1 = grid_direct_light(receiver, normal, sworld,
                                  stationary.light_mat, 0.9);
    TASK_CHECK(close(g0, g1, 1e-12));

    vec3 query(0.1, -0.2, 1.0);
    TASK_CHECK(close(static_light->pdf_value(receiver, query, 0.3),
                     static_light->pdf_value(receiver, query), 1e-12));
    return 0;
}

struct parsed_ppm {
    int width, height, maxv;
    std::vector<int> bytes;
    double mean() const {
        long sum = 0;
        for (size_t i = 0; i < bytes.size(); i++) sum += bytes[i];
        return double(sum) / bytes.size();
    }
};

static parsed_ppm parse_ppm(const std::string& data) {
    parsed_ppm out;
    std::istringstream in(data);
    std::string magic;
    in >> magic >> out.width >> out.height >> out.maxv;
    if (magic != "P3") { out.width = -1; return out; }
    for (int i = 0; i < out.width * out.height * 3; i++) {
        int v;
        in >> v;
        out.bytes.push_back(v);
    }
    return out;
}

// Full camera.render PPM against an independent integral averaged over the shutter.
static int transport_ppm_vs_shutter() {
    const point3 receiver(0, 0, -3.999);
    const vec3 normal(0, 0, 1);
    const double floor_albedo = 0.8;

    transport_scene scene = make_scene(true, false);
    hittable_list world, lights;
    world.add(scene.floor);
    world.add(scene.light_inst);
    lights.add(scene.light_inst);

    // Reference direct irradiance at points covering the camera's small footprint,
    // averaged over the whole shutter interval.
    const point3 footprint[5] = {
        receiver,
        receiver + vec3( 0.02,  0.02, 0),
        receiver + vec3(-0.02,  0.02, 0),
        receiver + vec3( 0.02, -0.02, 0),
        receiver + vec3(-0.02, -0.02, 0)
    };
    const int shutter_steps = 9;
    double shutter_average = 0.0;
    for (int f = 0; f < 5; f++) {
        for (int k = 0; k < shutter_steps; k++) {
            double t = double(k) / (shutter_steps - 1);
            shutter_average += grid_direct_light(footprint[f], normal, world,
                                                 scene.light_mat, t, 30, 60);
        }
    }
    shutter_average /= (5.0 * shutter_steps);
    // max_depth = 2 makes the render a pure direct-light estimate scaled by albedo.
    double predicted_radiance = floor_albedo * shutter_average;
    double predicted_byte = 256.0 * std::fmin(0.999,
                                    std::fmax(0.0, std::sqrt(predicted_radiance)));

    // Narrow field looking straight down: every pixel ray lands inside the footprint.
    camera cam;
    cam.image_width = 8;
    cam.aspect_ratio = 1;
    cam.samples_per_pixel = 16;
    cam.max_depth = 2;
    cam.background = color(0, 0, 0);
    cam.lookfrom = point3(0, 0, -3.5);
    cam.lookat = point3(0, 0, -4.0);
    cam.vup = vec3(0, 1, 0);
    cam.vfov = 6;
    cam.focus_dist = 0.5;
    cam.defocus_angle = 0;
    cam.enable_time_lights = true;

    std::ostringstream out;
    auto* old_buf = std::cout.rdbuf(out.rdbuf());
    cam.render(world, lights);
    std::cout.rdbuf(old_buf);

    parsed_ppm img = parse_ppm(out.str());
    TASK_CHECK(img.width == 8 && img.height == 8 && img.maxv == 255);
    TASK_CHECK((int)img.bytes.size() == 8 * 8 * 3);
    double rendered_mean = img.mean();
    double mean_error = std::fabs(rendered_mean - predicted_byte);
    std::cout << "PPM samples=" << (8 * 8 * 16)
              << " reference_byte=" << predicted_byte
              << " rendered_mean_byte=" << rendered_mean
              << " mean_error=" << mean_error << "\n";
    TASK_CHECK(rendered_mean > 5.0);                  // not black
    TASK_CHECK(mean_error < 18.0);

    // Occluded render must be visibly darker than the unoccluded one.
    transport_scene blocked = make_scene(true, true);
    hittable_list bworld, blights;
    bworld.add(blocked.floor);
    bworld.add(blocked.occluder);
    bworld.add(blocked.light_inst);
    blights.add(blocked.light_inst);
    std::ostringstream bout;
    old_buf = std::cout.rdbuf(bout.rdbuf());
    cam.render(bworld, blights);
    std::cout.rdbuf(old_buf);
    parsed_ppm blocked_img = parse_ppm(bout.str());
    std::cout << "OCCLUDED samples=" << (8 * 8 * 16)
              << " rendered_mean_byte=" << blocked_img.mean() << "\n";
    TASK_CHECK(blocked_img.mean() < rendered_mean - 3.0);

    // No-transform control: a static light render matches a static grid reference.
    double I[3][3]; mat_identity(I);
    point3 mid(0, 0, -2);
    auto static_light = affine_instance::make(
        make_shared<quad>(point3(-0.75, 0.75, 0), vec3(1.5, 0, 0), vec3(0, -1.5, 0),
                          scene.light_mat),
        I, mid, mid);
    TASK_CHECK(static_light != nullptr);
    hittable_list sworld, slights;
    sworld.add(scene.floor);
    sworld.add(static_light);
    slights.add(static_light);
    double static_ref = 0.0;
    for (int f = 0; f < 5; f++)
        static_ref += grid_direct_light(footprint[f], normal, sworld,
                                        scene.light_mat, 0.5, 30, 60);
    static_ref /= 5.0;
    double static_byte = 256.0 * std::fmin(0.999,
                                  std::fmax(0.0, std::sqrt(floor_albedo * static_ref)));

    std::ostringstream sout;
    old_buf = std::cout.rdbuf(sout.rdbuf());
    cam.render(sworld, slights);
    std::cout.rdbuf(old_buf);
    parsed_ppm static_img = parse_ppm(sout.str());
    double static_error = std::fabs(static_img.mean() - static_byte);
    std::cout << "STATIC samples=" << (8 * 8 * 16)
              << " reference_byte=" << static_byte
              << " rendered_mean_byte=" << static_img.mean()
              << " mean_error=" << static_error << "\n";
    TASK_CHECK(static_img.mean() > 5.0);
    TASK_CHECK(static_error < 18.0);

    // The moving and static exposures are distinct physical setups.
    TASK_CHECK(std::fabs(shutter_average - static_ref) > 1e-4);
    return 0;
}


// ==============================================================================================
// Registration
// ==============================================================================================

const struct task_case task_cases[] = {
    {"geometry", "sheared_ellipsoid_ray_normal_uv",      geometry_sheared_ellipsoid},
    {"geometry", "mirror_negative_determinant",          geometry_mirror_orientation},
    {"geometry", "nested_translate_rotate_motion",       geometry_nested_transforms},
    {"geometry", "bvh_full_shutter_coverage",            geometry_bvh_shutter_coverage},
    {"geometry", "shared_source_stays_still",            geometry_shared_source},
    {"geometry", "reject_singular_nonfinite_input",      geometry_reject_bad_input},

    {"density",  "solid_angle_jacobian_cap_difference",  density_jacobian_cap},
    {"density",  "histogram_and_sphere_normalization",   density_normalization},
    {"density",  "wrapper_sampling_forwards_time",       density_wrapper_time_forwarding},
    {"density",  "motion_two_instance_list_legacy",      density_motion_list_legacy},
    {"density",  "zero_nonfinite_illegal_time_guards",   density_query_guards},

    {"transport", "single_ray_time_consistency",         transport_single_ray_consistency},
    {"transport", "multi_ray_proposal_times",            transport_multi_ray_times},
    {"transport", "fixed_time_pdf_vs_grid_integral",     transport_fixed_time_integral},
    {"transport", "rendered_ppm_vs_shutter_integral",    transport_ppm_vs_shutter},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
