/*
 * relief material checks: analytic bilinear ray intersections, the deep
 * fragment callback, order independent occlusion through the real model and
 * graphics entry points, plus small PPM fixtures.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "graphics.h"
#include "mesh.h"
#include "relief_shader.h"

/* bit-cast construction keeps NaN/Inf available even under -ffast-math */
static float make_nan(void) {
    union { unsigned int bits; float value; } pun;
    pun.bits = 0x7fc00000u;
    return pun.value;
}

static float make_infinity(void) {
    union { unsigned int bits; float value; } pun;
    pun.bits = 0x7f800000u;
    return pun.value;
}

static int approx(float a, float b, float scale) {
    float tol = 1e-5f * float_max(1.0f, scale);
    return fabsf(a - b) <= tol;
}

static int approx_vec3(vec3_t a, vec3_t b, float scale) {
    return approx(a.x, b.x, scale) &&
           approx(a.y, b.y, scale) &&
           approx(a.z, b.z, scale);
}

/* independent bilinear reference used by the cross-check tests */
static float ref_height(const float *heights, int w, int h, float u, float v,
                        float *dhdu, float *dhdv) {
    int cx = (int)floor(u * (w - 1));
    int cy = (int)floor(v * (h - 1));
    float s, t, c00, c10, c01, c11;
    if (cx > w - 2) {
        cx = w - 2;
    }
    if (cy > h - 2) {
        cy = h - 2;
    }
    s = u * (w - 1) - cx;
    t = v * (h - 1) - cy;
    c00 = heights[cy * w + cx];
    c10 = heights[cy * w + cx + 1];
    c01 = heights[(cy + 1) * w + cx];
    c11 = heights[(cy + 1) * w + cx + 1];
    *dhdu = ((c10 - c00) * (1 - t) + (c11 - c01) * t) * (w - 1);
    *dhdv = ((c01 - c00) * (1 - s) + (c11 - c10) * s) * (h - 1);
    return (c00 * (1 - s) + c10 * s) * (1 - t) +
           (c01 * (1 - s) + c11 * s) * t;
}

static vec3_t unit(vec3_t v) {
    return vec3_normalize(v);
}

/* ------------------------------------------------------------------ */
/* intersection tests                                                  */
/* ------------------------------------------------------------------ */

static void test_constant_height(void) {
    float heights[9];
    relief_material_t material;
    relief_hit_t hit;
    int i;
    for (i = 0; i < 9; i++) {
        heights[i] = 0.5f;
    }
    material.width = 3;
    material.height = 3;
    material.heights = heights;
    material.scale = 0.8f;
    material.basecolor = vec3_new(1, 1, 1);
    material.near_plane = 0.1f;
    material.far_plane = 100.0f;

    assert(relief_intersect(&material,
                            vec3_new(0.3f, 0.4f, 1.0f),
                            unit(vec3_new(0, 0, -1)), &hit));
    assert(hit.hit);
    assert(approx_vec3(hit.position, vec3_new(0.3f, 0.4f, -0.4f), 0.8f));
    assert(approx(hit.uv.x, 0.3f, 0.8f) && approx(hit.uv.y, 0.4f, 0.8f));
    assert(approx_vec3(hit.normal, vec3_new(0, 0, 1), 0.8f));
    puts("relief constant height plane: ok");
}

static void test_slope(void) {
    float heights[4] = {0, 1, 0, 1};          /* h(u, v) = u */
    relief_material_t material;
    relief_hit_t hit;
    vec3_t expected_normal;
    material.width = 2;
    material.height = 2;
    material.heights = heights;
    material.scale = 0.5f;
    material.basecolor = vec3_new(1, 1, 1);
    material.near_plane = 0.1f;
    material.far_plane = 100.0f;

    assert(relief_intersect(&material,
                            vec3_new(0.2f, 0.7f, 0.6f),
                            unit(vec3_new(0, 0, -1)), &hit));
    assert(approx_vec3(hit.position, vec3_new(0.2f, 0.7f, -0.1f), 0.5f));
    expected_normal = vec3_normalize(vec3_new(0.5f, 0, 1));
    assert(approx_vec3(hit.normal, expected_normal, 0.5f));

    /* oblique ray crossing every cell of a wider slope grid */
    {
        float wide[12];
        relief_material_t wide_material;
        relief_hit_t wide_hit;
        int k;
        vec3_t origin = vec3_new(0.05f, 0.1f, 0.9f);
        vec3_t direction = unit(vec3_new(0.3f, 0.2f, -1.0f));
        float lambda = 0;
        int crossed = 0;
        wide_material.width = 4;
        wide_material.height = 3;
        wide_material.heights = wide;
        wide_material.scale = 0.7f;
        wide_material.basecolor = vec3_new(1, 1, 1);
        wide_material.near_plane = 0.1f;
        wide_material.far_plane = 100.0f;
        for (k = 0; k < 12; k++) {
            int gx = k % 4;
            wide[k] = (float)gx / 3.0f;       /* still h = u */
        }
        assert(relief_intersect(&wide_material, origin, direction, &wide_hit));
        /* independent analytic solve of z = -scale * u along the line */
        for (k = 0; k < 300000; k++) {
            float z = origin.z + lambda * direction.z;
            float u = origin.x + lambda * direction.x;
            float x = origin.x + lambda * direction.x;
            float y = origin.y + lambda * direction.y;
            if (x < 0 || x > 1 || y < 0 || y > 1 || z < -0.7f) {
                break;
            }
            if (z + wide_material.scale * u <= 0) {
                break;
            }
            lambda += 1e-5f;
        }
        assert(lambda > 0 && lambda < 3.0f);
        {
            float lo = lambda - 1e-5f, hi = lambda;
            int k2;
            if (lo < 0) {
                lo = 0;
            }
            for (k2 = 0; k2 < 60; k2++) {
                float mid = (lo + hi) * 0.5f;
                float z = origin.z + mid * direction.z;
                float u = origin.x + mid * direction.x;
                if (z + wide_material.scale * u > 0) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            lambda = (lo + hi) * 0.5f;
        }
        {
            vec3_t expected = vec3_new(origin.x + lambda * direction.x,
                                       origin.y + lambda * direction.y,
                                       origin.z + lambda * direction.z);
            float dhdu, dhdv;
            vec3_t normal;
            ref_height(wide, 4, 3, wide_hit.uv.x, wide_hit.uv.y, &dhdu, &dhdv);
            normal = vec3_normalize(
                vec3_new(0.7f * dhdu, 0.7f * dhdv, 1));
            assert(approx_vec3(wide_hit.position, expected, 0.7f));
            assert(approx_vec3(wide_hit.normal, normal, 0.7f));
            if (wide_hit.uv.x > 0.3f) {
                crossed = 1;
            }
        }
        assert(crossed);
    }
    puts("relief slope and multi-cell traversal: ok");
}

static void test_multi_cell_bump(void) {
    /* 4x4 smooth bump, checked against an independent bracket + bisection */
    float heights[16];
    relief_material_t material;
    relief_hit_t hit;
    vec3_t origin = vec3_new(0.12f, 0.08f, 1.1f);
    vec3_t direction = unit(vec3_new(0.35f, 0.22f, -1.0f));
    float lambda = 0;
    int i, j;
    material.width = 4;
    material.height = 4;
    material.heights = heights;
    material.scale = 0.9f;
    material.basecolor = vec3_new(1, 1, 1);
    material.near_plane = 0.1f;
    material.far_plane = 100.0f;
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++) {
            float u = (float)i / 3.0f;
            float v = (float)j / 3.0f;
            heights[j * 4 + i] =
                0.25f + 0.75f * sinf(PI * u) * sinf(PI * v);
        }
    }

    assert(relief_intersect(&material, origin, direction, &hit));
    assert(hit.uv.x >= 0 && hit.uv.x <= 1 && hit.uv.y >= 0 && hit.uv.y <= 1);

    for (i = 0; i < 200000; i++) {
        float x = origin.x + lambda * direction.x;
        float y = origin.y + lambda * direction.y;
        float z = origin.z + lambda * direction.z;
        float dhdu, dhdv, h;
        if (x < 0 || x > 1 || y < 0 || y > 1 || z < -material.scale) {
            break;
        }
        h = ref_height(heights, 4, 4, x, y, &dhdu, &dhdv);
        if (z + material.scale * h <= 0) {
            break;
        }
        lambda += 1e-5f;
    }
    {
        float lo = lambda - 1e-5f, hi = lambda;
        int k;
        for (k = 0; k < 60; k++) {
            float mid = (lo + hi) * 0.5f;
            float x = origin.x + mid * direction.x;
            float y = origin.y + mid * direction.y;
            float z = origin.z + mid * direction.z;
            float dhdu, dhdv, h =
                ref_height(heights, 4, 4, x, y, &dhdu, &dhdv);
            if (z + material.scale * h > 0) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        lambda = (lo + hi) * 0.5f;
    }
    {
        vec3_t expected = vec3_new(origin.x + lambda * direction.x,
                                   origin.y + lambda * direction.y,
                                   origin.z + lambda * direction.z);
        float dhdu, dhdv;
        vec3_t normal;
        ref_height(heights, 4, 4, hit.uv.x, hit.uv.y, &dhdu, &dhdv);
        normal = vec3_normalize(
            vec3_new(material.scale * dhdu, material.scale * dhdv, 1));
        assert(approx_vec3(hit.position, expected, material.scale));
        assert(approx_vec3(hit.normal, normal, material.scale));
        assert(approx(hit.position.z,
                      -material.scale *
                          ref_height(heights, 4, 4, hit.uv.x, hit.uv.y,
                                     &dhdu, &dhdv),
                      material.scale));
    }
    puts("relief multi-cell bilinear bump: ok");
}

static void test_double_roots(void) {
    /*
     * single cell H(s, t) = s*t, ray line (before normalization)
     *     s = 0.2 + 0.5 l, t = 0.3 + 0.4 l, z = -0.05 - 0.33 l
     * f(l) = 0.2 l^2 - 0.1 l + 0.01 has two admissible roots;
     * the earliest is l = 0.1381966...
     */
    float heights[4] = {0, 0, 0, 1};
    relief_material_t material;
    relief_hit_t hit;
    vec3_t origin = vec3_new(0.2f, 0.3f, -0.05f);
    vec3_t direction = unit(vec3_new(0.5f, 0.4f, -0.33f));
    float lambda = 0.1381966011250105f;
    vec3_t expected = vec3_new(0.2f + 0.5f * lambda,
                               0.3f + 0.4f * lambda,
                               -0.05f - 0.33f * lambda);
    material.width = 2;
    material.height = 2;
    material.heights = heights;
    material.scale = 1.0f;
    material.basecolor = vec3_new(1, 1, 1);
    material.near_plane = 0.1f;
    material.far_plane = 100.0f;

    assert(relief_intersect(&material, origin, direction, &hit));
    assert(approx_vec3(hit.position, expected, 1.0f));
    assert(approx(hit.uv.x, expected.x, 1.0f));
    assert(approx(hit.uv.y, expected.y, 1.0f));
    assert(hit.position.z < -0.09f && hit.position.z > -0.10f);
    puts("relief in-cell double roots, earliest selected: ok");
}

static void test_tangent(void) {
    /*
     * cell H(s, t) = s*t; the ray below is the grazing (discriminant zero)
     * construction. float normalization perturbs the exact zero slightly,
     * which is precisely the near-tangent regime the solver must clamp.
     * the expected contact is the analytic parabola vertex -q/(2p) built
     * independently from the normalized direction
     */
    float heights[4] = {0, 0, 0, 1};
    relief_material_t material;
    relief_hit_t hit;
    vec3_t origin = vec3_new(0.2f, 0.3f, -0.0475f);
    vec3_t raw = vec3_new(0.5f, 0.4f, -0.33f);
    vec3_t direction = unit(raw);
    double dx = direction.x, dy = direction.y, dz = direction.z;
    double h0 = origin.x * origin.y;
    double h1 = dx * origin.y + origin.x * dy;
    double h2 = dx * dy;
    double p = h2, q = dz + h1, r = origin.z + h0;
    double lambda = -q / (2 * p);
    assert(fabs(q * q - 4 * p * r) <= 1e-7);     /* analytically near tangent */
    vec3_t expected = vec3_new((float)(origin.x + lambda * dx),
                               (float)(origin.y + lambda * dy),
                               (float)(origin.z + lambda * dz));
    material.width = 2;
    material.height = 2;
    material.heights = heights;
    material.scale = 1.0f;
    material.basecolor = vec3_new(1, 1, 1);
    material.near_plane = 0.1f;
    material.far_plane = 100.0f;

    assert(relief_intersect(&material, origin, direction, &hit));
    assert(approx_vec3(hit.position, expected, 1.0f));
    /* the contact lies exactly on the bilinear surface */
    assert(approx(hit.position.z, -hit.uv.x * hit.uv.y, 1.0f));
    /* grazing contact: ray direction is parallel to the tangent plane */
    assert(fabsf(vec3_dot(hit.normal, direction)) <= 1e-4f);
    puts("relief analytic tangent contact: ok");
}

static void test_side_exit(void) {
    float heights[9];
    relief_material_t material;
    relief_hit_t hit;
    int i;
    for (i = 0; i < 9; i++) {
        heights[i] = 0.8f;                  /* surface stays at z = -0.8 */
    }
    material.width = 3;
    material.height = 3;
    material.heights = heights;
    material.scale = 1.0f;
    material.basecolor = vec3_new(1, 1, 1);
    material.near_plane = 0.1f;
    material.far_plane = 100.0f;

    /* horizontal ray above the surface leaves through a side */
    assert(!relief_intersect(&material,
                             vec3_new(0.5f, 0.5f, -0.02f),
                             unit(vec3_new(1, 0, 0)), &hit));
    assert(!hit.hit);

    /* ray entering one side and leaving the opposite side, no wrap-around */
    assert(!relief_intersect(&material,
                             vec3_new(-0.5f, 0.5f, -0.5f),
                             unit(vec3_new(1, 0, 0)), &hit));
    assert(!hit.hit);
    puts("relief side exits discard without wrapping: ok");
}

static void test_edge_rays(void) {
    /* 3x3 constant field: cell boundaries sit exactly on u or v = 0.5 */
    float heights[9];
    relief_material_t material;
    relief_hit_t hit;
    int i;
    for (i = 0; i < 9; i++) {
        heights[i] = 0.5f;
    }
    material.width = 3;
    material.height = 3;
    material.heights = heights;
    material.scale = 0.5f;
    material.basecolor = vec3_new(1, 1, 1);
    material.near_plane = 0.1f;
    material.far_plane = 100.0f;

    /* ray running along the shared edge v = 0.5, above the surface:
     * it must leave through a side instead of wrapping to another cell */
    assert(!relief_intersect(&material,
                             vec3_new(0.1f, 0.5f, -0.1f),
                             unit(vec3_new(1, 0, 0)), &hit));
    assert(!hit.hit);

    /* vertical ray striking the surface exactly on the shared edge u = 0.5 */
    assert(relief_intersect(&material,
                            vec3_new(0.5f, 0.3f, 0.4f),
                            unit(vec3_new(0, 0, -1)), &hit));
    assert(approx_vec3(hit.position, vec3_new(0.5f, 0.3f, -0.25f), 0.5f));
    assert(approx_vec3(hit.normal, vec3_new(0, 0, 1), 0.5f));

    /* oblique descent crossing a shared cell boundary while u stays at 0.5 */
    assert(relief_intersect(&material,
                            vec3_new(0.5f, 0.1f, 0.4f),
                            unit(vec3_new(0, 0.8f, -1.0f)), &hit));
    assert(approx(hit.position.x, 0.5f, 0.5f));
    assert(approx(hit.position.z, -0.25f, 0.5f));
    assert(hit.uv.y > 0.5f);               /* really entered the next cell row */
    puts("relief shared-edge rays do not wrap or stick: ok");
}

static unsigned int lcg_state = 0x12345678u;

static float lcg_float(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return (float)(lcg_state >> 8) / (float)(1u << 24);
}

static void test_fuzz_accuracy(void) {
    /* fixed-seed fuzz against an independent dense scan + bisection */
    int trial;
    for (trial = 0; trial < 300; trial++) {
        float grid[25];
        relief_material_t material;
        relief_hit_t hit;
        int w = 2 + (int)(lcg_float() * 4);
        int h = 2 + (int)(lcg_float() * 4);
        float scale = 0.05f + lcg_float() * 1.95f;
        vec3_t origin = vec3_new(lcg_float(), lcg_float(),
                                 0.05f + lcg_float() * 0.5f);
        vec3_t direction = unit(vec3_new(lcg_float() * 1.2f - 0.6f,
                                         lcg_float() * 1.2f - 0.6f, -1.0f));
        double lo, hi, t;
        int k, found = 0;
        material.width = w;
        material.height = h;
        material.heights = grid;
        material.scale = scale;
        material.basecolor = vec3_new(1, 1, 1);
        material.near_plane = 0.1f;
        material.far_plane = 100.0f;
        for (k = 0; k < w * h; k++) {
            grid[k] = lcg_float();
        }

        /* independent column interval */
        lo = 0;
        hi = 1e9;
        {
            double bounds[3][2] = {{0, 1}, {0, 1}, {-scale, 0}};
            double od[3] = {origin.x, origin.y, origin.z};
            double dd[3] = {direction.x, direction.y, direction.z};
            for (k = 0; k < 3; k++) {
                double t1, t2, tmin, tmax;
                if (fabs(dd[k]) < 1e-12) {
                    if (od[k] < bounds[k][0] || od[k] > bounds[k][1]) {
                        tmin = 1;
                        tmax = -1;
                    } else {
                        tmin = -1e9;
                        tmax = 1e9;
                    }
                } else {
                    t1 = (bounds[k][0] - od[k]) / dd[k];
                    t2 = (bounds[k][1] - od[k]) / dd[k];
                    tmin = t1 < t2 ? t1 : t2;
                    tmax = t1 < t2 ? t2 : t1;
                }
                if (tmin > lo) {
                    lo = tmin;
                }
                if (tmax < hi) {
                    hi = tmax;
                }
            }
        }
        if (lo < 0) {
            lo = 0;
        }

        t = hi;
        if (hi > lo) {
            int steps = 200000;
            double prev_lambda = lo;
            for (k = 0; k <= steps; k++) {
                double lambda = lo + (hi - lo) * k / steps;
                double x = origin.x + lambda * direction.x;
                double y = origin.y + lambda * direction.y;
                double z = origin.z + lambda * direction.z;
                float dhdu, dhdv;
                double f;
                if (x < 0 || x > 1 || y < 0 || y > 1) {
                    prev_lambda = lambda;
                    continue;
                }
                f = z + scale * ref_height(grid, w, h, (float)x, (float)y,
                                           &dhdu, &dhdv);
                if (f <= 0) {
                    double a = prev_lambda, b = lambda;
                    int j;
                    for (j = 0; j < 80; j++) {
                        double mid = (a + b) * 0.5;
                        double mx = origin.x + mid * direction.x;
                        double my = origin.y + mid * direction.y;
                        double mz = origin.z + mid * direction.z;
                        float du2, dv2;
                        double mf = mz + scale *
                            ref_height(grid, w, h, (float)mx, (float)my,
                                       &du2, &dv2);
                        if (mf > 0) {
                            a = mid;
                        } else {
                            b = mid;
                        }
                    }
                    t = (a + b) * 0.5;
                    found = 1;
                    break;
                }
                prev_lambda = lambda;
            }
        }

        if (found) {
            vec3_t expected = vec3_new(
                (float)(origin.x + t * direction.x),
                (float)(origin.y + t * direction.y),
                (float)(origin.z + t * direction.z));
            float dhdu, dhdv;
            vec3_t normal;
            assert(relief_intersect(&material, origin, direction, &hit));
            assert(approx_vec3(hit.position, expected, scale));
            ref_height(grid, w, h, hit.uv.x, hit.uv.y, &dhdu, &dhdv);
            normal = vec3_normalize(
                vec3_new(scale * dhdu, scale * dhdv, 1));
            assert(approx_vec3(hit.normal, normal, scale));
            /* residual of the analytic surface equation */
            assert(fabsf(hit.position.z +
                         scale * ref_height(grid, w, h, hit.uv.x, hit.uv.y,
                                            &dhdu, &dhdv))
                   <= 1e-5f * float_max(1.0f, scale));
        } else if (relief_intersect(&material, origin, direction, &hit)) {
            /* the coarse scan can only miss a grazing kiss: accept a hit
             * that lies on the surface and is tangent to the ray */
            float dhdu, dhdv;
            float residual = fabsf(
                hit.position.z + scale *
                ref_height(grid, w, h, hit.uv.x, hit.uv.y, &dhdu, &dhdv));
            assert(residual <= 2e-5f * float_max(1.0f, scale));
            assert(fabsf(vec3_dot(hit.normal, direction)) <= 1e-3f);
        }
    }
    puts("relief fuzz accuracy against bisection reference: ok");
}

static void test_zero_height_base(void) {
    float heights[9] = {0};
    relief_material_t material;
    relief_hit_t hit;
    material.width = 3;
    material.height = 3;
    material.heights = heights;
    material.scale = 0.7f;
    material.basecolor = vec3_new(1, 1, 1);
    material.near_plane = 0.1f;
    material.far_plane = 100.0f;

    /* zero height field with nonzero scale: exact base-plane hit at z = 0 */
    assert(relief_intersect(&material,
                            vec3_new(0.2f, 0.8f, 0.5f),
                            unit(vec3_new(0.1f, -0.05f, -1.0f)), &hit));
    assert(approx(hit.position.z, 0.0f, 0.7f));
    assert(approx_vec3(hit.normal, vec3_new(0, 0, 1), 0.7f));
    assert(hit.uv.x >= 0 && hit.uv.x <= 1 && hit.uv.y >= 0 && hit.uv.y <= 1);
    puts("relief zero height hits the base plane: ok");
}

static void test_flat_fallback(void) {
    float heights[4] = {0.2f, 0.4f, 0.6f, 0.8f};
    relief_material_t material;
    relief_hit_t hit;
    material.width = 2;
    material.height = 2;
    material.heights = heights;
    material.scale = 0.0f;
    material.basecolor = vec3_new(1, 1, 1);
    material.near_plane = 0.1f;
    material.far_plane = 100.0f;

    assert(relief_intersect(&material,
                            vec3_new(0.3f, 0.6f, 0.7f),
                            unit(vec3_new(0, 0, -1)), &hit));
    assert(approx_vec3(hit.position, vec3_new(0.3f, 0.6f, 0), 1.0f));
    assert(approx_vec3(hit.normal, vec3_new(0, 0, 1), 1.0f));
    puts("relief scale zero falls back to the base plane: ok");
}

static void test_invalid_material(void) {
    float good[4] = {0, 0.5f, 0.5f, 1};
    relief_material_t base;
    relief_material_t material;
    model_t *model;
    mat4_t identity = mat4_identity();

    base.width = 2;
    base.height = 2;
    base.heights = good;
    base.scale = 0.5f;
    base.basecolor = vec3_new(1, 1, 1);
    base.near_plane = 0.1f;
    base.far_plane = 100.0f;

    model = relief_create_model(&base, identity);
    assert(model != NULL);
    model->release(model);

    material = base;
    material.width = 1;
    assert(relief_create_model(&material, identity) == NULL);
    material = base;
    material.height = 1;
    assert(relief_create_model(&material, identity) == NULL);
    material = base;
    material.scale = -0.1f;
    assert(relief_create_model(&material, identity) == NULL);
    material = base;
    material.scale = make_infinity();
    assert(relief_create_model(&material, identity) == NULL);
    material = base;
    material.heights = NULL;
    assert(relief_create_model(&material, identity) == NULL);
    {
        float bad[4] = {0, 0.5f, 1.01f, 1};
        material = base;
        material.heights = bad;
        assert(relief_create_model(&material, identity) == NULL);
        bad[2] = -0.01f;
        assert(relief_create_model(&material, identity) == NULL);
        bad[2] = make_nan();
        assert(relief_create_model(&material, identity) == NULL);
    }
    material = base;
    material.far_plane = material.near_plane;
    assert(relief_create_model(&material, identity) == NULL);
    material = base;
    material.near_plane = 0;
    assert(relief_create_model(&material, identity) == NULL);
    puts("relief invalid dimensions and values rejected: ok");
}

/* ------------------------------------------------------------------ */
/* deep fragment callback tests                                        */
/* ------------------------------------------------------------------ */

typedef struct {vec4_t position;} deep_attribs_t;
typedef struct {float value;} deep_varyings_t;
typedef struct {
    vec4_t color;
    float wanted_depth;
    int discard;
    int calls;
} deep_uniforms_t;

static vec4_t deep_vs(void *attribs_, void *varyings_, void *uniforms_) {
    deep_attribs_t *attribs = (deep_attribs_t*)attribs_;
    deep_varyings_t *varyings = (deep_varyings_t*)varyings_;
    UNUSED_VAR(uniforms_);
    varyings->value = 1;
    return attribs->position;
}

static fragment_output_t deep_fs(void *varyings_, void *uniforms_,
                                 int backface, float base_depth) {
    deep_varyings_t *varyings = (deep_varyings_t*)varyings_;
    deep_uniforms_t *uniforms = (deep_uniforms_t*)uniforms_;
    fragment_output_t output;
    UNUSED_VAR(varyings);
    UNUSED_VAR(backface);
    UNUSED_VAR(base_depth);
    uniforms->calls++;
    output.color = uniforms->color;
    output.depth = uniforms->wanted_depth;
    output.discard = uniforms->discard;
    return output;
}

static void deep_draw(framebuffer_t *framebuffer, program_t *program,
                      float ndc_z, vec4_t color, float wanted_depth,
                      int discard) {
    static const float xy[3][2] = {
        {-1.2f, -0.8f}, {1.2f, -0.8f}, {0, 0.9f},
    };
    deep_uniforms_t *uniforms;
    int i;
    for (i = 0; i < 3; i++) {
        deep_attribs_t *attribs =
            (deep_attribs_t*)program_get_attribs(program, i);
        attribs->position = vec4_new(xy[i][0], xy[i][1], ndc_z, 1);
    }
    uniforms = (deep_uniforms_t*)program_get_uniforms(program);
    uniforms->color = color;
    uniforms->wanted_depth = wanted_depth;
    uniforms->discard = discard;
    graphics_draw_triangle(framebuffer, program);
}

static void test_deep_callback(void) {
    framebuffer_t *framebuffer = framebuffer_create(24, 24);
    program_t *program = program_create_deep(
        deep_vs, deep_fs,
        sizeof(deep_attribs_t), sizeof(deep_varyings_t),
        sizeof(deep_uniforms_t), 1, 0);
    deep_uniforms_t *uniforms;
    int index = 12 * 24 + 12;

    /* far base fragment (window depth 0.8) pulled to the front at 0.2 */
    deep_draw(framebuffer, program, 0.6f, vec4_new(1, 0, 0, 1), 0.2f, 0);
    assert(fabsf(framebuffer->depth_buffer[index] - 0.2f) < 1e-6f);
    assert(framebuffer->color_buffer[index * 4] == 255);

    /* closer base fragment but shader pushes it farther: rejected by <= */
    deep_draw(framebuffer, program, -0.8f, vec4_new(0, 1, 0, 1), 0.9f, 0);
    assert(fabsf(framebuffer->depth_buffer[index] - 0.2f) < 1e-6f);
    assert(framebuffer->color_buffer[index * 4] == 255);
    assert(framebuffer->color_buffer[index * 4 + 1] == 0);

    /* discard leaves color and depth untouched */
    uniforms = (deep_uniforms_t*)program_get_uniforms(program);
    uniforms->calls = 0;
    deep_draw(framebuffer, program, 0.6f, vec4_new(0, 0, 1, 1), 0.05f, 1);
    assert(fabsf(framebuffer->depth_buffer[index] - 0.2f) < 1e-6f);
    assert(framebuffer->color_buffer[index * 4 + 2] == 0);
    assert(uniforms->calls > 0);           /* shader still ran, no early-z */

    /* non-finite or out of range depths are dropped */
    deep_draw(framebuffer, program, 0.6f, vec4_new(0, 0, 1, 1), make_nan(), 0);
    deep_draw(framebuffer, program, 0.6f, vec4_new(0, 0, 1, 1), -0.5f, 0);
    deep_draw(framebuffer, program, 0.6f, vec4_new(0, 0, 1, 1), 1.5f, 0);
    assert(fabsf(framebuffer->depth_buffer[index] - 0.2f) < 1e-6f);
    assert(framebuffer->color_buffer[index * 4 + 2] == 0);

    program_release(program);
    framebuffer_release(framebuffer);
    puts("deep callback depth override, discard and invalid depth: ok");
}

/* old-style callback keeps its early depth rejection */
typedef struct {vec4_t position;} legacy_attribs_t;
typedef struct {float value;} legacy_varyings_t;
typedef struct {vec4_t color; int discard; int calls;} legacy_uniforms_t;

static vec4_t legacy_vs(void *attribs_, void *varyings_, void *uniforms_) {
    legacy_attribs_t *attribs = (legacy_attribs_t*)attribs_;
    legacy_varyings_t *varyings = (legacy_varyings_t*)varyings_;
    UNUSED_VAR(uniforms_);
    varyings->value = 1;
    return attribs->position;
}

static vec4_t legacy_fs(void *varyings_, void *uniforms_,
                        int *discard, int backface) {
    legacy_uniforms_t *uniforms = (legacy_uniforms_t*)uniforms_;
    UNUSED_VAR(varyings_);
    UNUSED_VAR(backface);
    uniforms->calls++;
    *discard = uniforms->discard;
    return uniforms->color;
}

static void test_legacy_early_reject(void) {
    framebuffer_t *framebuffer = framebuffer_create(24, 24);
    program_t *program = program_create(
        legacy_vs, legacy_fs,
        sizeof(legacy_attribs_t), sizeof(legacy_varyings_t),
        sizeof(legacy_uniforms_t), 1, 0);
    legacy_uniforms_t *uniforms =
        (legacy_uniforms_t*)program_get_uniforms(program);
    static const float xy[3][2] = {
        {-1.2f, -0.8f}, {1.2f, -0.8f}, {0, 0.9f},
    };
    int index = 12 * 24 + 12;
    int before, i;

    for (i = 0; i < 3; i++) {
        legacy_attribs_t *attribs =
            (legacy_attribs_t*)program_get_attribs(program, i);
        attribs->position = vec4_new(xy[i][0], xy[i][1], -0.4f, 1);
    }
    uniforms->color = vec4_new(1, 0, 0, 1);
    uniforms->discard = 0;
    graphics_draw_triangle(framebuffer, program);
    assert(fabsf(framebuffer->depth_buffer[index] - 0.3f) < 1e-6f);

    before = uniforms->calls;
    for (i = 0; i < 3; i++) {
        legacy_attribs_t *attribs =
            (legacy_attribs_t*)program_get_attribs(program, i);
        attribs->position = vec4_new(xy[i][0], xy[i][1], 0.8f, 1);
    }
    graphics_draw_triangle(framebuffer, program);
    assert(uniforms->calls == before);    /* far fragment never shaded */
    assert(fabsf(framebuffer->depth_buffer[index] - 0.3f) < 1e-6f);

    program_release(program);
    framebuffer_release(framebuffer);
    puts("legacy fragment shader keeps early depth rejection: ok");
}

/* ------------------------------------------------------------------ */
/* full pipeline: real model, real graphics, occluding ordinary geometry */
/* ------------------------------------------------------------------ */

typedef struct {vec3_t position;} occ_attribs_t;
typedef struct {float value;} occ_varyings_t;
typedef struct {mat4_t vp; mat4_t model; vec4_t color;} occ_uniforms_t;

static vec4_t occ_vs(void *attribs_, void *varyings_, void *uniforms_) {
    occ_attribs_t *attribs = (occ_attribs_t*)attribs_;
    occ_varyings_t *varyings = (occ_varyings_t*)varyings_;
    occ_uniforms_t *uniforms = (occ_uniforms_t*)uniforms_;
    vec4_t world = mat4_mul_vec4(uniforms->model,
                                 vec4_from_vec3(attribs->position, 1));
    varyings->value = 1;
    return mat4_mul_vec4(uniforms->vp, world);
}

static vec4_t occ_fs(void *varyings_, void *uniforms_,
                     int *discard, int backface) {
    occ_uniforms_t *uniforms = (occ_uniforms_t*)uniforms_;
    UNUSED_VAR(varyings_);
    UNUSED_VAR(backface);
    *discard = 0;
    return uniforms->color;
}

static model_t *create_occluder(mat4_t transform) {
    vertex_t *vertices = (vertex_t*)malloc(sizeof(vertex_t) * 6);
    vec3_t positions[6] = {
        {0.35f, 0.35f, -0.2f}, {0.65f, 0.35f, -0.2f},
        {0.65f, 0.65f, -0.2f}, {0.35f, 0.35f, -0.2f},
        {0.65f, 0.65f, -0.2f}, {0.35f, 0.65f, -0.2f},
    };
    model_t *model = (model_t*)malloc(sizeof(model_t));
    program_t *program = program_create(
        occ_vs, occ_fs,
        sizeof(occ_attribs_t), sizeof(occ_varyings_t),
        sizeof(occ_uniforms_t), 0, 0);
    occ_uniforms_t *uniforms;
    int i;
    for (i = 0; i < 6; i++) {
        vertices[i].position = positions[i];
        vertices[i].texcoord = vec2_new(0, 0);
        vertices[i].normal = vec3_new(0, 0, 1);
        vertices[i].tangent = vec4_new(1, 0, 0, 1);
        vertices[i].joint = vec4_new(0, 0, 0, 0);
        vertices[i].weight = vec4_new(0, 0, 0, 0);
    }
    uniforms = (occ_uniforms_t*)program_get_uniforms(program);
    uniforms->color = vec4_new(0.1f, 0.9f, 0.2f, 1);

    model->mesh = mesh_create(vertices, 2);
    model->program = program;
    model->transform = transform;
    model->skeleton = NULL;
    model->attached = -1;
    model->opaque = 1;
    model->distance = 0;
    model->update = NULL;
    model->draw = NULL;
    model->release = NULL;
    return model;
}

static void occluder_draw(model_t *model, framebuffer_t *framebuffer,
                          int shadow_pass) {
    occ_uniforms_t *uniforms =
        (occ_uniforms_t*)program_get_uniforms(model->program);
    vertex_t *vertices = mesh_get_vertices(model->mesh);
    int num_faces = mesh_get_num_faces(model->mesh);
    int i, j;
    UNUSED_VAR(shadow_pass);
    for (i = 0; i < num_faces; i++) {
        for (j = 0; j < 3; j++) {
            occ_attribs_t *attribs =
                (occ_attribs_t*)program_get_attribs(model->program, j);
            attribs->position = vertices[i * 3 + j].position;
        }
        graphics_draw_triangle(framebuffer, model->program);
    }
    UNUSED_VAR(uniforms);
}

static void occluder_update(model_t *model, perframe_t *perframe) {
    occ_uniforms_t *uniforms =
        (occ_uniforms_t*)program_get_uniforms(model->program);
    uniforms->vp = mat4_mul_mat4(perframe->camera_proj_matrix,
                                 perframe->camera_view_matrix);
    uniforms->model = model->transform;
}

static void occluder_release(model_t *model) {
    program_release(model->program);
    mesh_release(model->mesh);
    free(model);
}

static void write_ppm(const char *filename, framebuffer_t *framebuffer) {
    FILE *file = fopen(filename, "wb");
    int num_pixels = framebuffer->width * framebuffer->height;
    int i;
    assert(file != NULL);
    fprintf(file, "P6\n%d %d\n255\n", framebuffer->width, framebuffer->height);
    for (i = 0; i < num_pixels; i++) {
        unsigned char rgb[3];
        rgb[0] = framebuffer->color_buffer[i * 4 + 0];
        rgb[1] = framebuffer->color_buffer[i * 4 + 1];
        rgb[2] = framebuffer->color_buffer[i * 4 + 2];
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}

static void build_perframe(perframe_t *perframe, int width, int height) {
    vec3_t eye = vec3_new(0.5f, 0.5f, 2.2f);
    vec3_t target = vec3_new(0.5f, 0.5f, 0.0f);
    mat4_t view = mat4_lookat(eye, target, vec3_new(0, 1, 0));
    mat4_t proj = mat4_perspective(TO_RADIANS(60),
                                   (float)width / (float)height,
                                   0.1f, 100.0f);
    memset(perframe, 0, sizeof(perframe_t));
    perframe->camera_pos = eye;
    perframe->camera_view_matrix = view;
    perframe->camera_proj_matrix = proj;
    perframe->light_dir = vec3_normalize(vec3_new(-0.4f, -0.7f, -0.6f));
    perframe->ambient_intensity = 0.25f;
    perframe->punctual_intensity = 0.75f;
}

static void emit_ppm(const char *dir, const char *name,
                     framebuffer_t *framebuffer) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    write_ppm(path, framebuffer);
    /* also leave a persistent copy next to the repository root */
    write_ppm(name, framebuffer);
}

static void test_pipeline_and_fixtures(const char *dir) {
    enum {WIDTH = 64, HEIGHT = 40};
    float heights[16];
    relief_material_t material;
    relief_material_t flat_material;
    mat4_t rotation = mat4_rotate_y(TO_RADIANS(35));
    mat4_t transform = rotation;
    model_t *relief, *flat, *occluder;
    perframe_t perframe;
    framebuffer_t *fb_a, *fb_b, *fb_relief_only, *fb_flat;
    framebuffer_t *fb_occ_only;
    int num_pixels = WIDTH * HEIGHT;
    int num_bytes_color = num_pixels * 4;
    int num_bytes_depth = num_pixels * (int)sizeof(float);
    int differ_color = 0, differ_depth = 0;
    int relief_pixels = 0, occluder_pixels = 0, occluded_pixels = 0;
    int relief_deeper = 0, relief_shallower = 0;
    int i;

    for (i = 0; i < 16; i++) {
        int gx = i % 4;
        int gy = i / 4;
        float u = (float)gx / 3.0f;
        float v = (float)gy / 3.0f;
        heights[i] = 0.15f + 0.85f * sinf(PI * u) * sinf(PI * v);
    }
    material.width = 4;
    material.height = 4;
    material.heights = heights;
    material.scale = 0.6f;
    material.basecolor = vec3_new(0.85f, 0.35f, 0.2f);
    material.near_plane = 0.1f;
    material.far_plane = 100.0f;

    flat_material = material;
    flat_material.scale = 0.0f;

    relief = relief_create_model(&material, transform);
    flat = relief_create_model(&flat_material, transform);
    occluder = create_occluder(transform);
    occluder->update = occluder_update;
    occluder->draw = occluder_draw;
    occluder->release = occluder_release;
    assert(relief && flat && occluder);

    build_perframe(&perframe, WIDTH, HEIGHT);
    relief->update(relief, &perframe);
    flat->update(flat, &perframe);
    occluder->update(occluder, &perframe);

    fb_a = framebuffer_create(WIDTH, HEIGHT);
    fb_b = framebuffer_create(WIDTH, HEIGHT);
    fb_relief_only = framebuffer_create(WIDTH, HEIGHT);
    fb_flat = framebuffer_create(WIDTH, HEIGHT);
    fb_occ_only = framebuffer_create(WIDTH, HEIGHT);

    /* order A: relief first, occluder second */
    relief->draw(relief, fb_a, 0);
    occluder->draw(occluder, fb_a, 0);

    /* order B: occluder first, relief second */
    occluder->draw(occluder, fb_b, 0);
    relief->draw(relief, fb_b, 0);

    /* single-object references */
    relief->draw(relief, fb_relief_only, 0);
    flat->draw(flat, fb_flat, 0);
    occluder->draw(occluder, fb_occ_only, 0);

    assert(memcmp(fb_a->color_buffer, fb_b->color_buffer,
                  num_bytes_color) == 0);
    assert(memcmp(fb_a->depth_buffer, fb_b->depth_buffer,
                  num_bytes_depth) == 0);

    /*
     * every pixel covered by the ordinary quad must show the quad, never
     * the recessed surface, regardless of the drawing order; the relief
     * itself must still be visible elsewhere and its true hit depths must
     * sit behind the base plane instead of reusing the base depth
     */
    for (i = 0; i < num_pixels; i++) {
        int covered_by_occluder = fb_occ_only->color_buffer[i * 4 + 1] > 200;
        int covered_by_relief =
            fb_relief_only->color_buffer[i * 4 + 0] > 0 ||
            fb_relief_only->color_buffer[i * 4 + 1] > 0 ||
            fb_relief_only->color_buffer[i * 4 + 2] > 0;
        if (covered_by_relief) {
            relief_pixels++;
        }
        if (covered_by_occluder) {
            int k;
            occluder_pixels++;
            for (k = 0; k < 3; k++) {
                assert(fb_a->color_buffer[i * 4 + k] ==
                       fb_occ_only->color_buffer[i * 4 + k]);
            }
            assert(fabsf(fb_a->depth_buffer[i] -
                         fb_occ_only->depth_buffer[i]) < 1e-7f);
            if (covered_by_relief) {
                occluded_pixels++;
                assert(fb_occ_only->depth_buffer[i] <
                       fb_relief_only->depth_buffer[i]);
            }
        }
        if (memcmp(&fb_relief_only->color_buffer[i * 4],
                   &fb_flat->color_buffer[i * 4], 3) != 0) {
            differ_color = 1;
        }
        if (fabsf(fb_relief_only->depth_buffer[i] -
                  fb_flat->depth_buffer[i]) > 1e-7f) {
            differ_depth = 1;
            if (fb_relief_only->depth_buffer[i] >
                fb_flat->depth_buffer[i]) {
                relief_deeper++;
            } else {
                relief_shallower++;
            }
        }
    }
    assert(relief_pixels > 0 && occluder_pixels > 0 && occluded_pixels > 0);
    assert(differ_color && differ_depth && relief_deeper > 0);
    assert(relief_shallower == 0);      /* recessed points are always farther */

    emit_ppm(dir, "relief.ppm", fb_relief_only);
    emit_ppm(dir, "relief_flat.ppm", fb_flat);

    framebuffer_release(fb_a);
    framebuffer_release(fb_b);
    framebuffer_release(fb_relief_only);
    framebuffer_release(fb_flat);
    framebuffer_release(fb_occ_only);
    relief->release(relief);
    flat->release(flat);
    occluder->release(occluder);
    puts("pipeline occlusion is order independent; ppm fixtures written");
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";

    test_constant_height();
    test_slope();
    test_multi_cell_bump();
    test_double_roots();
    test_tangent();
    test_side_exit();
    test_edge_rays();
    test_zero_height_base();
    test_flat_fallback();
    test_invalid_material();
    test_fuzz_accuracy();

    test_deep_callback();
    test_legacy_early_reject();

    test_pipeline_and_fixtures(dir);

    puts("ALL RELIEF CHECKS PASSED");
    return 0;
}
