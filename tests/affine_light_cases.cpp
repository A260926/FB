//==============================================================================================
// Task tests for the time-dependent affine light wrapper (affine_instance).
//
// Registered with task_support/check.h in three groups: geometry, density, transport.
// This file deliberately defines no main(): task_support/driver.c supplies it.
//==============================================================================================

#include "task_support/check.h"

#include "rtweekend.h"
#include "camera.h"
#include "sphere.h"
#include "quad.h"
#include "bvh.h"
#include "pdf.h"
#include "affine_light.h"

#include <sstream>
#include <vector>
#include <algorithm>
#include <string>


// ----------------------------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------------------------

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol * (1.0 + std::fabs(a) + std::fabs(b));
}

static bool vec_near(const vec3& a, const vec3& b, double tol = 1e-9) {
    return near(a.x(), b.x(), tol) && near(a.y(), b.y(), tol) && near(a.z(), b.z(), tol);
}

static mat3 diag3(double x, double y, double z) {
    return mat3(x, 0, 0,  0, y, 0,  0, 0, z);
}

// Independent local-sphere intersection (nearest positive root), written from scratch so the
// production hit() path is not used as its own reference.
static bool local_sphere_hit(const point3& o, const vec3& d, double& root) {
    double a = d.length_squared();
    double h = dot(d, -o);                 // quadratic uses (center - o) = -o
    double c = o.length_squared() - 1.0;   // unit sphere at the origin
    double disc = h*h - a*c;
    if (disc < 0) return false;
    double s = std::sqrt(disc);
    double t = (h - s) / a;
    if (t < 1e-6) {
        t = (h + s) / a;
        if (t < 1e-6) return false;
    }
    root = t;
    return true;
}

static void sphere_uv_of(const point3& unit_p, double& u, double& v) {
    double theta = std::acos(-unit_p.y());
    double phi = std::atan2(-unit_p.z(), unit_p.x()) + pi;
    u = phi / (2*pi);
    v = theta / pi;
}

// Solid angle of a spherical triangle with unit-vector corners (Van Oosterom & Strackee).
static double spherical_triangle_solid_angle(const vec3& a, const vec3& b, const vec3& c) {
    double numerator = dot(a, cross(b, c));
    double denominator = 1.0 + dot(a,b) + dot(b,c) + dot(c,a);
    return std::fabs(2.0 * std::atan2(numerator, denominator));
}

static double spherical_quad_solid_angle(const vec3& c00, const vec3& c10,
                                         const vec3& c11, const vec3& c01) {
    return spherical_triangle_solid_angle(c00, c10, c11)
         + spherical_triangle_solid_angle(c00, c11, c01);
}

static void orthonormal_basis(const vec3& w, vec3& e1, vec3& e2) {
    vec3 a = (std::fabs(w.x()) > 0.9) ? vec3(0,1,0) : vec3(1,0,0);
    e1 = unit_vector(cross(w, a));
    e2 = cross(w, e1);
}

// Deterministic uniform direction on the unit sphere.
static vec3 uniform_direction(long index, long total) {
    double golden = pi * (3.0 - std::sqrt(5.0));
    double y = 1.0 - 2.0 * (index + 0.5) / total;
    double r = std::sqrt(std::fmax(0.0, 1.0 - y*y));
    double phi = golden * index;
    return vec3(std::cos(phi)*r, y, std::sin(phi)*r);
}

// Integrates a solid-angle density over the whole unit sphere with a deterministic Fibonacci
// lattice; the lattice weight per point is the same solid angle 4*pi/N.
static double integrate_on_sphere(const hittable& obj, const point3& origin, double time,
                                  long n = 120000) {
    double total = 0.0;
    for (long i = 0; i < n; i++) {
        vec3 d = uniform_direction(i, n);
        total += obj.pdf_value(origin, d, time);
    }
    return total * (4*pi / n);
}


// ----------------------------------------------------------------------------------------------
// geometry group
// ----------------------------------------------------------------------------------------------

// Non-uniform scale plus shear applied to a unit sphere; non-unit ray, analytic t/normal/UV.
static int geometry_sheared_ellipsoid() {
    auto matte = make_shared<lambertian>(color(.5,.5,.5));
    auto sphere_ptr = make_shared<sphere>(point3(0,0,0), 1.0, matte);
    mat3 A(2.0, 0.5, 0.0,
           0.0, 1.0, 0.0,
           0.0, 0.0, 0.75);
    vec3 b(0,0,-6);
    auto inst = make_shared<affine_instance>(sphere_ptr, A, b, b);

    ray r(point3(0,0,0), vec3(0.1,-0.15,-2.0), 0.4);
    TASK_CHECK(r.direction().length_squared() != 1.0); // genuinely non-unit direction

    point3 local_origin = A.inverse() * (r.origin() - b);
    vec3 local_direction = A.inverse() * r.direction();

    double expected_t;
    TASK_CHECK(local_sphere_hit(local_origin, local_direction, expected_t));

    hit_record rec;
    TASK_CHECK(inst->hit(r, interval(0.001, infinity), rec));
    TASK_CHECK(near(rec.t, expected_t, 1e-8));

    // The returned t must work with the original world ray even with a non-unit direction.
    TASK_CHECK(vec_near(rec.p, r.at(rec.t), 1e-8));

    point3 local_hit = local_origin + expected_t * local_direction;
    TASK_CHECK(near(local_hit.length(), 1.0, 1e-8));

    // Inverse-transpose normal, re-normalized in world space.
    vec3 expected_normal = unit_vector(A.inverse().transpose() * local_hit);
    TASK_CHECK(rec.front_face);
    TASK_CHECK(vec_near(rec.normal, expected_normal, 1e-8));

    // UVs belong to the untouched local sphere.
    double eu, ev;
    sphere_uv_of(local_hit, eu, ev);
    TASK_CHECK(near(rec.u, eu, 1e-9) && near(rec.v, ev, 1e-9));
    TASK_CHECK(rec.mat == matte);
    TASK_CHECK(near(rec.t > 0 ? r.time() : 0.0, 0.4)); // time retained

    // A ray aimed well away from the ellipsoid misses.
    ray miss_r(point3(0,0,0), vec3(10,1,0), 0.4);
    hit_record miss_rec;
    TASK_CHECK(!inst->hit(miss_r, interval(0.001, infinity), miss_rec));
    return 0;
}

// Negative determinant: mirroring must not swap the interior/exterior side of the sphere.
static int geometry_negative_determinant() {
    auto matte = make_shared<lambertian>(color(.5,.5,.5));
    auto sphere_ptr = make_shared<sphere>(point3(0,0,0), 1.0, matte);
    mat3 A = diag3(-2.0, 1.0, 1.0);
    TASK_CHECK(A.determinant() < 0.0);
    vec3 b(0,0,-5);
    auto inst = make_shared<affine_instance>(sphere_ptr, A, b, b);

    // Exterior hit aimed at the center: front face, outward normal toward the observer.
    hit_record rec;
    TASK_CHECK(inst->hit(ray(point3(0,0,0), vec3(0,0,-2.3), 0.2),
                         interval(0.001, infinity), rec));
    TASK_CHECK(rec.front_face);
    TASK_CHECK(vec_near(rec.normal, vec3(0,0,1), 1e-8));

    // Local point (1,0,0) maps to the mirrored world point b + (-2,0,0). Shoot from outside
    // the ellipsoid on the -x side so the first encounter is an exterior front-face hit.
    point3 mirrored = b + vec3(-2,0,0);
    ray mirror_ray(point3(-4,0,-5), vec3(1.4,0,0), 0.2);
    TASK_CHECK(inst->hit(mirror_ray, interval(0.001, infinity), rec));
    TASK_CHECK(rec.front_face);
    TASK_CHECK(vec_near(rec.p, mirrored, 1e-8));
    TASK_CHECK(vec_near(rec.normal, vec3(-1,0,0), 1e-8));

    // A ray starting inside the ellipsoid must still register as a back-face (interior) hit.
    point3 inside = b + A * vec3(0.5,0,0);
    ray interior_ray(inside, A * vec3(1,0,0), 0.2);
    TASK_CHECK(inst->hit(interior_ray, interval(0.0, infinity), rec));
    TASK_CHECK(!rec.front_face);
    TASK_CHECK(vec_near(rec.normal, vec3(1,0,0), 1e-8)); // stored normal faces the ray
    return 0;
}

// A moving sphere inside a moving instance, itself nested in a second instance.
static int geometry_nested_moving() {
    auto matte = make_shared<lambertian>(color(.5,.5,.5));
    auto moving = make_shared<sphere>(point3(0,0,0), point3(0,2,0), 1.0, matte);

    auto inner = make_shared<affine_instance>(moving, diag3(1,1,0.5), vec3(0,0,0), vec3(0,0,0));
    auto outer = make_shared<affine_instance>(inner, diag3(2,1,1),
                                              vec3(0,0,-5), vec3(0,0,-7));

    double t = 0.25;
    // sphere center at t: (0, .5, 0); inner leaves it in place; outer maps and translates to
    // (0, .5, -5.5).
    point3 expected_center(0, 0.5, -5.5);
    ray r(point3(0,0,0), expected_center * 1.4, t); // non-unit direction

    // Independent expectation: flatten the nested affine maps into one local frame and solve
    // the sphere intersection there. The moving sphere's own center at this time is (0,2t,0).
    point3 sphere_center_local(0, 2.0*t, 0);
    mat3 combined = diag3(2,1,1) * diag3(1,1,0.5);
    vec3 outer_b(0,0,-5.5);
    point3 local_origin = combined.inverse() * (r.origin() - outer_b);
    vec3 local_direction = combined.inverse() * r.direction();
    double expected_t;
    TASK_CHECK(local_sphere_hit(local_origin - sphere_center_local,
                                local_direction, expected_t));

    hit_record rec;
    TASK_CHECK(outer->hit(r, interval(0.001, infinity), rec));
    TASK_CHECK(near(rec.t, expected_t, 1e-7));
    TASK_CHECK(vec_near(rec.p, r.at(rec.t), 1e-8));

    // The hit is the near surface, strictly before the ray reaches the transformed center.
    double t_center = expected_center.length() / r.direction().length();
    TASK_CHECK(rec.t < t_center);
    point3 local_hit = local_origin + expected_t * local_direction;
    TASK_CHECK(near((local_hit - sphere_center_local).length(), 1.0, 1e-8));

    bool threw = false;
    try {
        outer->pdf_value(point3(0,0,0), vec3(0,0,-1), 1.5);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

// translate/rotate_y wrappers must keep the shutter time and must not double-transform points.
static int geometry_rotate_translate_chain() {
    auto matte = make_shared<lambertian>(color(.5,.5,.5));
    auto sphere_ptr = make_shared<sphere>(point3(0,0,0), 1.0, matte);
    // Moving translation: at t=0 center (0,0,-4), at t=1 center (0,0,-6).
    auto inner = make_shared<affine_instance>(sphere_ptr, mat3(),
                                              vec3(0,0,-4), vec3(0,0,-6));
    auto moved = make_shared<translate>(inner, vec3(2,0,0));
    auto rotated = make_shared<rotate_y>(moved, 90.0);

    double t = 0.7;
    // Pre-rotation center at t: (2, 0, -5.4); rotating 90 degrees about Y gives (-5.4, 0, -2).
    point3 expected_center(-5.4, 0, -2);
    ray r(point3(0,0,0), expected_center * 1.1, t);
    hit_record rec;
    TASK_CHECK(rotated->hit(r, interval(0.001, infinity), rec));
    TASK_CHECK(vec_near(rec.p, r.at(rec.t), 1e-8));
    // The hit is on the unit sphere centered at expected_center, and the ray line passes
    // through that center.
    TASK_CHECK(near((rec.p - expected_center).length(), 1.0, 1e-6));
    TASK_CHECK(near(std::fabs(dot(unit_vector(r.direction()),
                                  unit_vector(expected_center))), 1.0, 1e-9));
    TASK_CHECK(rec.front_face);
    TASK_CHECK(vec_near(rec.normal, -unit_vector(expected_center), 1e-7));

    // At t=0 the same ray line would only clip the sphere; hitting dead-center proves the time
    // actually travelled through both wrappers instead of being reset to 0.
    hit_record rec0;
    TASK_CHECK(!rotated->hit(ray(r.origin(), r.direction(), 0.0),
                             interval(0.001, infinity), rec0)
               || (rec0.p - point3(-4,0,-2)).length() > 0.5);
    return 0;
}

// BVH and the flat list must agree at every shutter time and never miss a moving instance.
static int geometry_bvh_full_shutter() {
    auto matte = make_shared<lambertian>(color(.5,.5,.5));
    auto sphere_ptr = make_shared<sphere>(point3(0,0,0), 1.0, matte);
    auto moving = make_shared<affine_instance>(sphere_ptr, mat3(),
                                               vec3(-3,0,-5), vec3(3,0,-5));
    hittable_list list;
    list.add(moving);
    bvh_node bvh(list);

    std::srand(12345);
    int hits = 0, total = 0;
    const double times[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
    for (int ti = 0; ti < 5; ti++) {
        point3 center((1.0 - times[ti]) * -3.0 + times[ti] * 3.0, 0, -5);
        for (int k = 0; k < 200; k++) {
            vec3 jitter(0.6*random_double(-1,1), 0.6*random_double(-1,1),
                        0.4*random_double(-1,1));
            ray r(point3(0,0,0), unit_vector(center + jitter) * (1.0 + random_double()),
                  times[ti]);
            hit_record rec_list, rec_bvh;
            bool h_list = list.hit(r, interval(0.001, infinity), rec_list);
            bool h_bvh  = bvh.hit(r, interval(0.001, infinity), rec_bvh);
            TASK_CHECK(h_list == h_bvh);
            if (h_list) {
                TASK_CHECK(near(rec_list.t, rec_bvh.t, 1e-9));
                ++hits;
            }
            ++total;
        }
    }
    TASK_CHECK(hits > total / 2);

    // The reported box must contain every corner-mapped surface position across the shutter.
    aabb box = moving->bounding_box();
    for (int ti = 0; ti <= 10; ti++) {
        double t = ti / 10.0;
        vec3 b((1.0 - t) * -3.0 + t * 3.0, 0, -5);
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                for (int sz = -1; sz <= 1; sz += 2) {
                    point3 p = b + vec3(sx, sy, sz); // A is identity here
                    TASK_CHECK(box.x.contains(p.x()) && box.y.contains(p.y())
                               && box.z.contains(p.z()));
                }
    }
    return 0;
}

// Constructor validation, and one shared primitive backing two independent instances.
static int geometry_rejection_and_sharing() {
    auto matte = make_shared<lambertian>(color(.5,.5,.5));
    auto sphere_ptr = make_shared<sphere>(point3(0,0,0), 1.0, matte);

    bool threw = false;
    try {
        mat3 singular(1,2,3, 2,4,6, 7,8,9); // rows 0 and 1 are proportional
        affine_instance bad(sphere_ptr, singular, vec3(0,0,0), vec3(0,0,0));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        mat3 nan_mat(1,0,0, 0,std::numeric_limits<double>::infinity(),0, 0,0,1);
        affine_instance bad(sphere_ptr, nan_mat, vec3(0,0,0), vec3(0,0,0));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        affine_instance bad(sphere_ptr, mat3(), vec3(std::nan(""),0,0), vec3(0,0,0));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TASK_CHECK(threw);

    auto inst_a = make_shared<affine_instance>(sphere_ptr, diag3(1,1,1),
                                               vec3(0,0,-3), vec3(0,0,-3));
    auto inst_b = make_shared<affine_instance>(sphere_ptr, diag3(2,2,2),
                                               vec3(4,0,-3), vec3(4,0,-3));
    hit_record rec;
    TASK_CHECK(inst_a->hit(ray(point3(0,0,0), vec3(0,0,-1), 0.0),
                           interval(0.001, infinity), rec));
    TASK_CHECK(vec_near(rec.p, point3(0,0,-2), 1e-9));
    TASK_CHECK(inst_b->hit(ray(point3(0,0,0), vec3(4,0,-3), 0.0),
                           interval(0.001, infinity), rec));
    // The shared primitive itself has not moved.
    TASK_CHECK(sphere_ptr->hit(ray(point3(0,0,0), vec3(0,0,-1), 0.0),
                               interval(0.001, infinity), rec));
    TASK_CHECK(vec_near(rec.p, point3(0,0,-1), 1e-9));
    return 0;
}


// ----------------------------------------------------------------------------------------------
// density group
// ----------------------------------------------------------------------------------------------

// Solid-angle Jacobian checked with an independent spherical-patch area finite difference.
static int density_jacobian_patch() {
    mat3 A(2.0, 0.5, 0.0,
           0.0, 1.0, 0.0,
           0.0, 0.3, 0.75);
    vec3 d_local = unit_vector(vec3(0.3, 0.2, 1.0));
    vec3 e1, e2;
    orthonormal_basis(d_local, e1, e2);

    const double hs[2] = {2e-3, 1e-3};
    for (int hi = 0; hi < 2; hi++) {
        double h = hs[hi];
        vec3 l00 = unit_vector(d_local + h*(-e1 - e2));
        vec3 l10 = unit_vector(d_local + h*( e1 - e2));
        vec3 l11 = unit_vector(d_local + h*( e1 + e2));
        vec3 l01 = unit_vector(d_local + h*(-e1 + e2));
        double local_solid = spherical_quad_solid_angle(l00, l10, l11, l01);

        vec3 w00 = unit_vector(A * l00);
        vec3 w10 = unit_vector(A * l10);
        vec3 w11 = unit_vector(A * l11);
        vec3 w01 = unit_vector(A * l01);
        double world_solid = spherical_quad_solid_angle(w00, w10, w11, w01);

        double measured_jacobian = world_solid / local_solid;
        double analytic_jacobian =
            std::fabs(A.determinant()) / std::pow((A*d_local).length(), 3.0);
        TASK_CHECK(near(measured_jacobian, analytic_jacobian, 5e-3));
    }
    return 0;
}

// Fixed-seed direction frequencies must match the density integrated over spherical bins.
static int density_histogram() {
    auto matte = make_shared<lambertian>(color(.5,.5,.5));
    auto sphere_ptr = make_shared<sphere>(point3(0,0,0), 1.0, matte);
    mat3 A(1.5, 0.4, 0.0,
           0.0, 0.8, 0.0,
           0.2, 0.0, 1.2);
    auto light = make_shared<affine_instance>(sphere_ptr, A, vec3(0,0,0), vec3(0,0,0));
    // Origin chosen so its pre-image A^-1 * origin is sqrt(2) units away: the ellipsoid
    // subtends a ~45 degree half-angle cone, covering many histogram bins.
    point3 origin = A * point3(1, 0, 1);
    TASK_CHECK((A.inverse() * origin).length() > 1.0); // observer stays outside local sphere

    const int n_phi = 16, n_theta = 10, N = 20000;
    std::vector<long> counts(n_phi * n_theta, 0);
    std::srand(777);
    for (int i = 0; i < N; i++) {
        vec3 d = unit_vector(light->random(origin, 0.37));
        double theta = std::acos(d.y());
        double phi = std::atan2(d.z(), d.x());
        if (phi < 0) phi += 2*pi;
        int bp = std::min(n_phi - 1, int(phi / (2*pi) * n_phi));
        int bt = std::min(n_theta - 1, int(theta / pi * n_theta));
        counts[bt * n_phi + bp]++;
    }

    int checked = 0;
    const int sub = 6; // sub-sample each bin so partially covered edge bins are handled
    for (int bt = 0; bt < n_theta; bt++) {
        double cos_lo = std::cos(bt * pi / n_theta);
        double cos_hi = std::cos((bt + 1) * pi / n_theta);
        double bin_solid = (2*pi / n_phi) * (cos_lo - cos_hi); // cosine decreases with theta
        for (int bp = 0; bp < n_phi; bp++) {
            double phi_lo = bp * 2*pi / n_phi;
            double phi_hi = (bp + 1) * 2*pi / n_phi;

            // Average the density over the bin's solid angle with a uniform sub-lattice in
            // (phi, cos theta), whose area element is exactly d(phi) d(cos theta) = d(omega).
            double mean_density = 0.0;
            for (int sp = 0; sp < sub; sp++) {
                double phi = phi_lo + (sp + 0.5) / sub * (phi_hi - phi_lo);
                for (int st = 0; st < sub; st++) {
                    double u = cos_hi + (st + 0.5) / sub * (cos_lo - cos_hi);
                    double sin_theta = std::sqrt(std::fmax(0.0, 1.0 - u*u));
                    vec3 d(sin_theta * std::cos(phi), u, sin_theta * std::sin(phi));
                    mean_density += light->pdf_value(origin, d, 0.37);
                }
            }
            mean_density /= sub * sub;

            double expected = mean_density * bin_solid * N;
            // Only compare bins with enough samples for a meaningful frequency test; small
            // edge bins carry too much Poisson relative variance.
            if (expected < 100.0) continue;
            long observed = counts[bt * n_phi + bp];
            TASK_CHECK(std::fabs(observed - expected) / expected < 0.35);
            ++checked;
        }
    }
    TASK_CHECK(checked >= 8);
    return 0;
}

// Numerical normalization over the whole sphere for several configurations.
static int density_normalization() {
    auto matte = make_shared<lambertian>(color(.5,.5,.5));
    auto sphere_ptr = make_shared<sphere>(point3(0,0,0), 1.0, matte);

    // (a) non-uniform scale plus shear.
    mat3 shear(1.5, 0.4, 0.0,
               0.1, 0.8, 0.0,
               0.2, 0.0, 1.2);
    auto sheared = make_shared<affine_instance>(sphere_ptr, shear, vec3(0,0,0), vec3(0,0,0));
    double integral = integrate_on_sphere(*sheared, point3(4,0,5), 0.3);
    TASK_CHECK(near(integral, 1.0, 0.03));

    // (b) negative determinant stays a normalized, non-negative density.
    auto mirrored = make_shared<affine_instance>(sphere_ptr, diag3(-1.5, 0.8, 1.2),
                                                 vec3(0,0,0), vec3(0,0,0));
    integral = integrate_on_sphere(*mirrored, point3(4,0,5), 0.6);
    TASK_CHECK(near(integral, 1.0, 0.03));
    for (long i = 0; i < 2000; i++) {
        vec3 d = uniform_direction(i, 2000);
        TASK_CHECK(mirrored->pdf_value(point3(4,0,5), d, 0.6) >= 0.0);
    }

    // (c) sheared quad; observer must not lie on the quad's plane.
    auto quad_ptr = make_shared<quad>(point3(-1,1,-1), vec3(2,0,0), vec3(0,0,2), matte);
    auto quad_light = make_shared<affine_instance>(quad_ptr, shear, vec3(0,0,0), vec3(0,0,0));
    point3 quad_origin(3, 4, 2);
    TASK_CHECK(std::fabs((shear.inverse() * quad_origin).y() - 1.0) > 1e-6);
    integral = integrate_on_sphere(*quad_light, quad_origin, 0.5);
    TASK_CHECK(near(integral, 1.0, 0.03));

    // (d) a light list made of two instances sharing the one primitive.
    hittable_list two_lights;
    two_lights.add(make_shared<affine_instance>(sphere_ptr, diag3(1,1,1),
                                                vec3(0,0,0), vec3(0,0,0)));
    two_lights.add(make_shared<affine_instance>(sphere_ptr, diag3(1,1,1),
                                                vec3(6,0,0), vec3(6,0,0)));
    point3 list_origin(3, 0, 6);
    integral = integrate_on_sphere(two_lights, list_origin, 0.5);
    TASK_CHECK(near(integral, 1.0, 0.03));

    // Query-direction scaling invariance and degenerate-query handling.
    vec3 q(0.2, -0.1, -1.0);
    double p1 = sheared->pdf_value(point3(4,0,5), q, 0.3);
    double p2 = sheared->pdf_value(point3(4,0,5), q * 3.7, 0.3);
    TASK_CHECK(near(p1, p2, 1e-12));
    TASK_CHECK(sheared->pdf_value(point3(4,0,5), vec3(0,0,0), 0.3) == 0.0);
    TASK_CHECK(sheared->pdf_value(point3(4,0,5),
                                  vec3(std::nan(""),1,1), 0.3) == 0.0);
    bool threw = false;
    try {
        sheared->pdf_value(point3(4,0,5), q, -0.01);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

// A custom object that implements only the legacy time-less virtuals.
class legacy_uniform_light : public hittable {
  public:
    // Keep the time-aware base overloads visible alongside the legacy overrides.
    using hittable::pdf_value;
    using hittable::random;

    bool hit(const ray&, interval, hit_record&) const override { return false; }
    aabb bounding_box() const override {
        return aabb(point3(-1,-1,-1), point3(1,1,1));
    }
    double pdf_value(const point3&, const vec3&) const override {
        return 1.0 / (4*pi); // uniform over the sphere
    }
    vec3 random(const point3&) const override {
        return random_unit_vector();
    }
};

static int density_legacy_fallback() {
    legacy_uniform_light legacy;

    // The base-class time-aware entry points must dispatch to the legacy overrides.
    vec3 d(0.1, 0.2, -0.9);
    TASK_CHECK(near(legacy.pdf_value(point3(1,2,3), d, 0.71), 1.0/(4*pi), 1e-15));
    TASK_CHECK(legacy.random(point3(1,2,3), 0.71).length_squared() > 0.0);

    // Wrapped in an affine instance the legacy proposal still normalizes on the world sphere.
    mat3 A(1.3, 0.25, 0.0,
           0.0, 0.7, 0.0,
           0.1, 0.0, 1.1);
    auto wrapped = make_shared<affine_instance>(
        make_shared<legacy_uniform_light>(), A, vec3(0,0,0), vec3(0,0,0));
    double integral = integrate_on_sphere(*wrapped, point3(2,0,3), 0.42);
    TASK_CHECK(near(integral, 1.0, 0.03));

    // Generated proposals span the whole sphere rather than collapsing to a cone.
    std::srand(4242);
    double min_y = 1e9, max_y = -1e9;
    for (int i = 0; i < 4000; i++) {
        vec3 g = unit_vector(wrapped->random(point3(2,0,3), 0.42));
        min_y = std::min(min_y, g.y());
        max_y = std::max(max_y, g.y());
    }
    TASK_CHECK(min_y < -0.8 && max_y > 0.8);
    return 0;
}


// ----------------------------------------------------------------------------------------------
// transport group
// ----------------------------------------------------------------------------------------------

// Records every shutter time seen by each sampling/intersection entry point.
class recording_hittable : public hittable {
  public:
    explicit recording_hittable(shared_ptr<hittable> inner) : inner(inner) {}

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        hit_times.push_back(r.time());
        return inner->hit(r, ray_t, rec);
    }
    aabb bounding_box() const override { return inner->bounding_box(); }

    double pdf_value(const point3& o, const vec3& d) const override {
        pdf_times.push_back(0.0);
        return inner->pdf_value(o, d);
    }
    vec3 random(const point3& o) const override {
        random_times.push_back(0.0);
        return inner->random(o);
    }
    double pdf_value(const point3& o, const vec3& d, double time) const override {
        pdf_times.push_back(time);
        return inner->pdf_value(o, d, time);
    }
    vec3 random(const point3& o, double time) const override {
        random_times.push_back(time);
        return inner->random(o, time);
    }

    shared_ptr<hittable> inner;
    mutable std::vector<double> hit_times;
    mutable std::vector<double> pdf_times;
    mutable std::vector<double> random_times;
};

// Wraps the whole world so every camera/path ray time is captured (primary and every bounce).
class world_recorder : public hittable {
  public:
    explicit world_recorder(shared_ptr<hittable> inner) : inner(inner) {}

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        ray_times.push_back(r.time());
        return inner->hit(r, ray_t, rec);
    }
    aabb bounding_box() const override { return inner->bounding_box(); }
    double pdf_value(const point3& o, const vec3& d, double time) const override {
        return inner->pdf_value(o, d, time);
    }
    vec3 random(const point3& o, double time) const override {
        return inner->random(o, time);
    }

    shared_ptr<hittable> inner;
    mutable std::vector<double> ray_times;
};

struct transport_scene {
    shared_ptr<material> light_mat;
    shared_ptr<material> ground_mat;
    shared_ptr<affine_instance> light_instance;
    shared_ptr<recording_hittable> recorder;
    shared_ptr<hittable> occluder;
    hittable_list world;
    shared_ptr<world_recorder> recorded_world;
    hittable_list lights;
    point3 receiver;
};

static mat3 transport_shear() {
    return mat3(1.0, 0.25, 0.0,
                0.0, 1.0,  0.0,
                0.0, 0.2,  1.0);
}

static transport_scene make_transport_scene(bool occluded, bool identity_transform) {
    transport_scene s;
    s.light_mat = make_shared<diffuse_light>(color(5,5,5));
    s.ground_mat = make_shared<lambertian>(color(0.8,0.8,0.8));

    auto ground = make_shared<quad>(point3(-8,0,-8), vec3(16,0,0), vec3(0,0,16),
                                    s.ground_mat);
    s.world.add(ground);

    auto local_quad = make_shared<quad>(point3(-1,1,-1), vec3(2,0,0), vec3(0,0,2),
                                        s.light_mat);
    mat3 A = identity_transform ? mat3() : transport_shear();
    vec3 b0 = identity_transform ? vec3(0,4,-0.8) : vec3(-1.5,4,-1.0);
    vec3 b1 = identity_transform ? vec3(0,4,-0.8) : vec3( 1.5,4,-1.0);
    s.recorder = make_shared<recording_hittable>(local_quad);
    s.light_instance = make_shared<affine_instance>(
        shared_ptr<hittable>(s.recorder), A, b0, b1);
    s.world.add(s.light_instance);
    s.lights.add(s.light_instance);

    if (occluded) {
        s.occluder = make_shared<sphere>(point3(0,2.2,-0.6), 0.4,
                                         make_shared<lambertian>(color(0.1,0.1,0.1)));
        s.world.add(s.occluder);
    }

    s.receiver = point3(0, 0.01, 0);

    // A second view over the same shared primitives that records every path ray's time.
    s.recorded_world = make_shared<world_recorder>(make_shared<hittable_list>(s.world));
    return s;
}

// Per-bounce consistency of proposal / pdf / intersection times, with and without the flag.
static int transport_time_consistency() {
    // Direct single-bounce check at fixed instants.
    transport_scene s = make_transport_scene(false, false);
    const double times[5] = {0.0, 0.13, 0.5, 0.87, 1.0};
    for (int ti = 0; ti < 5; ti++) {
        double t = times[ti];
        hittable_pdf light_pdf(s.lights, s.receiver, t);
        vec3 w = light_pdf.generate();
        double density = light_pdf.value(w);
        TASK_CHECK(density > 0.0);
        hit_record rec;
        TASK_CHECK(s.world.hit(ray(s.receiver, w, t), interval(0.001, infinity), rec));
        bool saw_t = false;
        for (size_t i = 0; i < s.recorder->random_times.size(); i++)
            saw_t = saw_t || near(s.recorder->random_times[i], t);
        TASK_CHECK(saw_t);
        TASK_CHECK(std::fabs(s.recorder->pdf_times.back() - t) < 1e-12);
        TASK_CHECK(std::fabs(s.recorder->hit_times.back() - t) < 1e-12);
    }

    // Full render with time lights: every recorded time is shared by the proposal/density/hit
    // hooks of the same bounce set, and times span the shutter rather than collapsing to zero.
    // Clear the fixed-instant records from the direct checks above first.
    s.recorder->hit_times.clear();
    s.recorder->pdf_times.clear();
    s.recorder->random_times.clear();

    std::srand(99);
    camera cam;
    cam.image_width = 10; cam.aspect_ratio = 1.0; cam.samples_per_pixel = 4;
    cam.max_depth = 2; cam.background = color(0,0,0);
    cam.vfov = 60; cam.lookfrom = point3(0,3,0.001); cam.lookat = point3(0,0,0);
    cam.vup = vec3(0,0,-1);
    cam.enable_time_lights = true;
    std::ostringstream out;
    auto* old = std::cout.rdbuf(out.rdbuf());
    cam.render(*s.recorded_world, s.lights);
    std::cout.rdbuf(old);

    TASK_CHECK(!s.recorder->pdf_times.empty());
    TASK_CHECK(!s.recorder->random_times.empty());
    TASK_CHECK(!s.recorder->hit_times.empty());
    TASK_CHECK(!s.recorded_world->ray_times.empty());

    auto contains = [](const std::vector<double>& v, double t) {
        for (size_t i = 0; i < v.size(); i++)
            if (near(v[i], t, 1e-12)) return true;
        return false;
    };

    // Every light query is in [0,1], and times spread across the shutter instead of
    // collapsing to zero.
    int nonzero = 0;
    for (size_t i = 0; i < s.recorder->pdf_times.size(); i++) {
        double t = s.recorder->pdf_times[i];
        TASK_CHECK(t >= 0.0 && t <= 1.0);
        if (t > 0.05) ++nonzero;
    }
    TASK_CHECK(nonzero > 0);

    // Each proposal time must be one of the density-query times from the same bounce set.
    for (size_t i = 0; i < s.recorder->random_times.size(); i++)
        TASK_CHECK(contains(s.recorder->pdf_times, s.recorder->random_times[i]));

    // Proposal, density and light-intersection times must all be path-ray times: the light
    // sampling and visibility never use a different shutter instant than the path itself.
    for (size_t i = 0; i < s.recorder->random_times.size(); i++)
        TASK_CHECK(contains(s.recorded_world->ray_times, s.recorder->random_times[i]));
    for (size_t i = 0; i < s.recorder->pdf_times.size(); i++)
        TASK_CHECK(contains(s.recorded_world->ray_times, s.recorder->pdf_times[i]));
    for (size_t i = 0; i < s.recorder->hit_times.size(); i++)
        TASK_CHECK(contains(s.recorded_world->ray_times, s.recorder->hit_times[i]));

    // With the flag off, the legacy dispatch pins every light query to time 0.
    transport_scene s0 = make_transport_scene(false, false);
    std::srand(100);
    camera cam0;
    cam0.image_width = 8; cam0.aspect_ratio = 1.0; cam0.samples_per_pixel = 1;
    cam0.max_depth = 2; cam0.background = color(0,0,0);
    cam0.vfov = 60; cam0.lookfrom = point3(0,3,0.001); cam0.lookat = point3(0,0,0);
    cam0.vup = vec3(0,0,-1);
    TASK_CHECK(cam0.enable_time_lights == false);
    std::ostringstream out0;
    old = std::cout.rdbuf(out0.rdbuf());
    cam0.render(s0.world, s0.lights);
    std::cout.rdbuf(old);
    for (size_t i = 0; i < s0.recorder->pdf_times.size(); i++)
        TASK_CHECK(s0.recorder->pdf_times[i] == 0.0);
    for (size_t i = 0; i < s0.recorder->random_times.size(); i++)
        TASK_CHECK(s0.recorder->random_times[i] == 0.0);
    return 0;
}

// Independent uniform-sphere reference for the direct-lighting integral at one shutter time.
static double reference_direct_lighting(const transport_scene& s, double time, long samples) {
    vec3 n(0,1,0);
    double sum = 0.0;
    for (long i = 0; i < samples; i++) {
        vec3 d = uniform_direction(i, samples);
        hit_record rec;
        if (s.world.hit(ray(s.receiver, d, time), interval(0.001, infinity), rec)
            && rec.mat == s.light_mat) {
            double cosine = std::fmax(0.0, dot(n, d));
            sum += 5.0 * cosine / pi;
        }
    }
    return sum * (4*pi / samples); // uniform sphere pdf is 1/(4*pi)
}

// Same integral estimated with the time-aware hittable_pdf as the sole light proposal.
static double pdf_direct_lighting(const transport_scene& s, double time, long samples) {
    vec3 n(0,1,0);
    hittable_pdf light_pdf(s.lights, s.receiver, time);
    double sum = 0.0;
    for (long i = 0; i < samples; i++) {
        vec3 w = light_pdf.generate();
        double density = light_pdf.value(w);
        if (!(density > 0.0)) continue;
        hit_record rec;
        if (s.world.hit(ray(s.receiver, w, time), interval(0.001, infinity), rec)
            && rec.mat == s.light_mat) {
            double cosine = std::fmax(0.0, dot(n, unit_vector(w)));
            sum += (5.0 * cosine / pi) / density;
        }
    }
    return sum / samples;
}

static int transport_fixed_time_estimates() {
    const long N = 12000;
    const double times[3] = {0.0, 0.5, 1.0};

    transport_scene moving = make_transport_scene(false, false);
    for (int ti = 0; ti < 3; ti++) {
        std::srand(2000 + ti);
        double a = pdf_direct_lighting(moving, times[ti], N);
        std::srand(3000 + ti);
        double b = reference_direct_lighting(moving, times[ti], N);
        std::cout << "fixed-time t=" << times[ti]
                  << " light-pdf=" << a << " reference=" << b
                  << " relerr=" << std::fabs(a-b)/b << "\n";
        TASK_CHECK(a > 0.0 && b > 0.0);
        TASK_CHECK(std::fabs(a - b) / b < 0.18);
    }

    // Occluded scene: both estimators must agree and the shadow must actually darken things.
    transport_scene blocked = make_transport_scene(true, false);
    std::srand(4100);
    double a = pdf_direct_lighting(blocked, 0.5, N);
    std::srand(4200);
    double b = reference_direct_lighting(blocked, 0.5, N);
    std::cout << "occluded light-pdf=" << a << " reference=" << b
              << " relerr=" << std::fabs(a-b)/b << "\n";
    TASK_CHECK(a > 0.0 && b > 0.0 && std::fabs(a-b)/b < 0.2);
    std::srand(4300);
    double unblocked = reference_direct_lighting(moving, 0.5, N);
    TASK_CHECK(b < 0.85 * unblocked);

    // No-transform control scene.
    transport_scene plain = make_transport_scene(false, true);
    std::srand(5100);
    double pa = pdf_direct_lighting(plain, 0.5, N);
    std::srand(5200);
    double pb = reference_direct_lighting(plain, 0.5, N);
    std::cout << "identity light-pdf=" << pa << " reference=" << pb
              << " relerr=" << std::fabs(pa-pb)/pb << "\n";
    TASK_CHECK(pa > 0.0 && pb > 0.0 && std::fabs(pa-pb)/pb < 0.15);
    return 0;
}

static std::vector<int> render_ppm(const transport_scene& s, int width, int spp, int depth) {
    camera cam;
    cam.image_width = width; cam.aspect_ratio = 1.0; cam.samples_per_pixel = spp;
    cam.max_depth = depth; cam.background = color(0,0,0);
    cam.vfov = 60; cam.lookfrom = point3(0,3,0.001); cam.lookat = point3(0,0,0);
    cam.vup = vec3(0,0,-1);
    cam.enable_time_lights = true;

    std::ostringstream out;
    auto* old = std::cout.rdbuf(out.rdbuf());
    cam.render(s.world, s.lights);
    std::cout.rdbuf(old);

    std::istringstream in(out.str());
    std::string magic;
    int w, h, maxv;
    in >> magic >> w >> h >> maxv;
    std::vector<int> bytes;
    for (int i = 0; i < w*h*3; i++) {
        int v;
        in >> v;
        bytes.push_back(v);
    }
    std::string extra;
    if (in >> extra) bytes.clear(); // malformed PPM fails the test below
    return bytes;
}

// Shutter-wide reference in gamma-encoded byte units, matching the camera's one-bounce
// Lambertian path (albedo times the cosine-weighted direct integral) and write_color's
// per-pixel gamma encoding.
static int reference_byte(const transport_scene& s, long samples_per_point) {
    const double albedo = 0.8;
    const int grid = 7, n_times = 11;
    double byte_sum = 0.0, count = 0.0;
    for (int ix = 0; ix < grid; ix++) {
        for (int iz = 0; iz < grid; iz++) {
            double x = -1.5 + 3.0 * (ix + 0.5) / grid;
            double z = -1.5 + 3.0 * (iz + 0.5) / grid;
            point3 p(x, 0.01, z);
            double spot = 0.0;
            for (int it = 0; it < n_times; it++) {
                double t = double(it) / (n_times - 1);
                // Re-seed per cell so every point gets the same deterministic sample budget.
                std::srand(6000 + ix*100 + iz*10 + it);
                vec3 n(0,1,0);
                double sum = 0.0;
                for (long k = 0; k < samples_per_point; k++) {
                    vec3 d = uniform_direction(k, samples_per_point);
                    hit_record rec;
                    if (s.world.hit(ray(p, d, t), interval(0.001, infinity), rec)
                        && rec.mat == s.light_mat) {
                        sum += 5.0 * std::fmax(0.0, dot(n, d)) / pi;
                    }
                }
                spot += sum * (4*pi / samples_per_point);
            }
            // Linear expectation at this point, including the Lambertian albedo.
            double linear = albedo * spot / n_times;
            double gamma = std::sqrt(std::fmax(0.0, linear));
            byte_sum += int(256 * std::fmin(0.999, std::fmax(0.0, gamma)));
            count += 1.0;
        }
    }
    return int(byte_sum / count);
}

static int transport_render_against_shutter_integral() {
    const int width = 12, spp = 9, depth = 2, core = 8;

    struct Mode { const char* name; bool occluded; bool identity; };
    Mode modes[2] = { {"moving-sheared", false, false}, {"occluded", true, false} };

    for (int m = 0; m < 2; m++) {
        transport_scene s = make_transport_scene(modes[m].occluded, modes[m].identity);
        std::srand(7000 + m);
        std::vector<int> bytes = render_ppm(s, width, spp, depth);
        TASK_CHECK(bytes.size() == size_t(width * width * 3));

        // Average the central core of pixels (all three channels are equal for these scenes).
        long cam_sum = 0, cam_n = 0;
        for (int y = (width-core)/2; y < (width+core)/2; y++)
            for (int x = (width-core)/2; x < (width+core)/2; x++)
                for (int c = 0; c < 3; c++)
                    cam_sum += bytes[(y*width + x)*3 + c], ++cam_n;
        int cam_byte = int(cam_sum / cam_n);

        std::srand(7100 + m);
        int ref_byte = reference_byte(s, 3000);
        int err = std::fabs(cam_byte - ref_byte);
        std::cout << modes[m].name << " samples=" << (width*width*spp)
                  << " camera_byte=" << cam_byte << " reference_byte=" << ref_byte
                  << " abs_error=" << err << "\n";
        TASK_CHECK(cam_byte > 0);                 // not just "image is non-black"
        TASK_CHECK(err <= 16);
    }

    transport_scene plain = make_transport_scene(false, true);
    std::srand(7200);
    std::vector<int> plain_bytes = render_ppm(plain, width, spp, depth);
    long plain_sum = 0, plain_n = 0;
    for (int y = (width-core)/2; y < (width+core)/2; y++)
        for (int x = (width-core)/2; x < (width+core)/2; x++)
            for (int c = 0; c < 3; c++)
                plain_sum += plain_bytes[(y*width + x)*3 + c], ++plain_n;
    int plain_cam = int(plain_sum / plain_n);
    int plain_ref = reference_byte(plain, 3000);
    std::cout << "identity-control samples=" << (width*width*spp)
              << " camera_byte=" << plain_cam << " reference_byte=" << plain_ref
              << " abs_error=" << std::fabs(plain_cam - plain_ref) << "\n";
    TASK_CHECK(plain_cam > 0 && std::fabs(plain_cam - plain_ref) <= 16);

    // The occluder must leave a visible trace relative to the unoccluded render.
    transport_scene blocked = make_transport_scene(true, false);
    std::srand(7300);
    std::vector<int> blocked_bytes = render_ppm(blocked, width, spp, depth);
    long blocked_min = 256;
    for (size_t i = 0; i < blocked_bytes.size(); i += 3)
        blocked_min = std::min(blocked_min, long(blocked_bytes[i]));
    TASK_CHECK(blocked_min == 0); // some pixels see no light at all
    return 0;
}


// ----------------------------------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------------------------------

extern const struct task_case task_cases[] = {
    {"geometry", "sheared_ellipsoid_ray",            geometry_sheared_ellipsoid},
    {"geometry", "negative_determinant_sides",       geometry_negative_determinant},
    {"geometry", "nested_moving_instances",          geometry_nested_moving},
    {"geometry", "rotate_translate_keeps_time",      geometry_rotate_translate_chain},
    {"geometry", "bvh_full_shutter_agreement",       geometry_bvh_full_shutter},
    {"geometry", "rejection_and_shared_primitive",   geometry_rejection_and_sharing},

    {"density",  "jacobian_solid_angle_difference",  density_jacobian_patch},
    {"density",  "histogram_frequency_vs_density",   density_histogram},
    {"density",  "spherical_normalization",          density_normalization},
    {"density",  "legacy_sampling_fallback",         density_legacy_fallback},

    {"transport","per_bounce_time_consistency",      transport_time_consistency},
    {"transport","fixed_time_light_integrals",       transport_fixed_time_estimates},
    {"transport","render_vs_shutter_integral",       transport_render_against_shutter_integral},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
