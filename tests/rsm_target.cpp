/*========================================================================================
 RSM task tests.

 Registered through task_support/check.h in four groups (capture, transport, camera,
 lifetime). No main() is defined here: task_support/driver.c drives the cases.

 Every numerical cross check uses an independent floating point reference (plain
 Sutherland-Hodgman NDC clipping, float edge functions, explicit perspective-correct
 barycentric weights and a hand written discrete transport sum) rather than the
 production rasterizer.
========================================================================================*/

#include "task_support/check.h"

#include <microgl/canvas.h>
#include <microgl/bitmaps/bitmap.h>
#include <microgl/pixel_coders/RGB888_PACKED_32.h>
#include <microgl/shaders/color_shader.h>
#include <microgl/z_buffer.h>
#include <microgl/rsm/Rsm.h>

#include <vector>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace {
    using number = float;
    using namespace microgl;
    using namespace microgl::rsm;
    using V3 = vertex3<number>;
    using V4 = vertex4<number>;
    using M4 = matrix_4x4<number>;

    const number kPi = number(3.14159265358979323846);

    bool near(number a, number b, number eps) {
        const number d = a > b ? a - b : b - a;
        return d <= eps;
    }

    bool near_vec(const V3 &a, const V3 &b, number eps) {
        return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
    }

    // relative comparison for quantities whose magnitude varies widely.
    bool near_rel(number a, number b, number rel) {
        const number scale = number(1) + (a > b ? a : b);
        return near(a, b, rel * scale);
    }

    M4 ortho(number l, number r, number b, number t, number n, number f) {
        M4 m;
        m.identity();
        m[0] = number(2) / (r - l);
        m[5] = number(2) / (t - b);
        m[10] = number(-2) / (f - n);
        m[12] = -(r + l) / (r - l);
        m[13] = -(t + b) / (t - b);
        m[14] = -(f + n) / (f - n);
        return m;
    }

    M4 perspective(number fovy_rad, number aspect, number n, number f) {
        M4 m;
        m.identity();
        const number tan_half = number(std::tan(fovy_rad / number(2)));
        m[0] = number(1) / (aspect * tan_half);
        m[5] = number(1) / tan_half;
        m[10] = -(f + n) / (f - n);
        m[11] = number(-1);
        m[14] = number(-2) * f * n / (f - n);
        m[15] = number(0);
        return m;
    }

    M4 translation(number x, number y, number z) {
        return M4::translate(x, y, z);
    }

    // ----------------------------------------------------------------------
    // Independent float reference for one orthographic capture.
    // ----------------------------------------------------------------------
    struct ref_vertex {
        V4 clip;
        V3 world;
        V3 normal;
    };

    struct ref_triangle {
        ref_vertex v[3];
        rsm_rgb<number> reflectance;
    };

    struct ref_sample {
        bool valid = false;
        V3 world{0, 0, 0};
        V3 normal{0, 0, 1};
        number depth = 0;
        bool front = false;
        rsm_rgb<number> flux{0, 0, 0};
        // float barycentric weights of the sample inside its clipped fan triangle;
        // a weight near 0 means the texel hugs an edge or the internal seam, where
        // fixed-point fill-rule quantization dominates.
        number w0 = 0, w1 = 0, w2 = 0;
    };

    V3 bary3(const V3 &a, const V3 &b, const V3 &c, number wa, number wb, number wc) {
        return V3{a.x * wa + b.x * wb + c.x * wc,
                  a.y * wa + b.y * wb + c.y * wc,
                  a.z * wa + b.z * wb + c.z * wc};
    }

    // accurate float normalization (the production code uses std::sqrt; the
    // library vec3::normalize is a coarse fixed-point approximation).
    V3 ref_normalize(const V3 &v) {
        const number len = number(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
        if (len == number(0)) return v;
        return V3{v.x / len, v.y / len, v.z / len};
    }

    // Sutherland-Hodgman clipping of a clip-space triangle against the NDC cube.
    // Orthographic light => every clip w is 1, so NDC clipping equals the canvas
    // six-plane homogeneous clip for these inputs.
    struct poly_vertex {
        V4 ndc;
        number bary[3];
    };

    poly_vertex lerp_poly(const poly_vertex &a, const poly_vertex &b, number t) {
        poly_vertex r;
        r.ndc = a.ndc + (b.ndc - a.ndc) * t;
        for (int i = 0; i < 3; ++i) r.bary[i] = a.bary[i] + (b.bary[i] - a.bary[i]) * t;
        return r;
    }

    void clip_axis(std::vector<poly_vertex> &poly, int axis, bool greater) {
        std::vector<poly_vertex> out;
        const size_t n = poly.size();
        for (size_t i = 0; i < n; ++i) {
            const poly_vertex &cur = poly[i];
            const poly_vertex &prv = poly[(i + n - 1) % n];
            const number cv = (&cur.ndc.x)[axis];
            const number pv = (&prv.ndc.x)[axis];
            const bool cin = greater ? (cv >= number(-1)) : (cv <= number(1));
            const bool pin = greater ? (pv >= number(-1)) : (pv <= number(1));
            if (cin) {
                if (!pin) {
                    const number t = greater ? (number(-1) - pv) / (cv - pv)
                                             : (number(1) - pv) / (cv - pv);
                    out.push_back(lerp_poly(prv, cur, t));
                }
                out.push_back(cur);
            } else if (pin) {
                const number t = greater ? (number(-1) - pv) / (cv - pv)
                                         : (number(1) - pv) / (cv - pv);
                out.push_back(lerp_poly(prv, cur, t));
            }
        }
        poly.swap(out);
    }

    // Independent nearest-surface light map, written with plain float edge
    // functions and the canonical top-left fill rule.
    std::vector<ref_sample> reference_capture(
            const std::vector<ref_triangle> &tris, const rsm_light<number> &light,
            unsigned N) {
        std::vector<ref_sample> map(N * N);
        const M4 vp = light.projection * light.view;
        const V3 ldir = ref_normalize(light.light_direction);
        const number texel_area =
                (light.world_width * light.world_height) / number(N * N);

        for (size_t ti = 0; ti < tris.size(); ++ti) {
            const ref_triangle &rt = tris[ti];
            std::vector<poly_vertex> poly(3);
            for (int k = 0; k < 3; ++k) {
                poly[k].ndc = rt.v[k].clip / rt.v[k].clip.w;
                poly[k].bary[0] = k == 0 ? 1 : 0;
                poly[k].bary[1] = k == 1 ? 1 : 0;
                poly[k].bary[2] = k == 2 ? 1 : 0;
            }
            clip_axis(poly, 0, true);
            clip_axis(poly, 0, false);
            clip_axis(poly, 1, true);
            clip_axis(poly, 1, false);
            clip_axis(poly, 2, true);
            clip_axis(poly, 2, false);
            if (poly.size() < 3) continue;

            for (size_t fi = 1; fi + 1 < poly.size(); ++fi) {
                const poly_vertex &pa = poly[0];
                const poly_vertex &pb = poly[fi];
                const poly_vertex &pc = poly[fi + 1];

                auto screen = [&](const poly_vertex &q) {
                    // continuous canvas raster coordinates, y flipped.
                    const number sx = (q.ndc.x + number(1)) * number(N) / number(2);
                    const number sy = number(N) - (q.ndc.y + number(1)) * number(N) / number(2);
                    return V3{sx, sy, (q.ndc.z + number(1)) / number(2)};
                };
                const V3 sa = screen(pa), sb = screen(pb), sc = screen(pc);
                const number area = (sb.x - sa.x) * (sc.y - sa.y) -
                                    (sb.y - sa.y) * (sc.x - sa.x);
                if (near(area, 0, number(1e-12))) continue;

                auto attr = [&](const poly_vertex &q, V3 &world, V3 &normal,
                                number &depth) {
                    world = bary3(rt.v[0].world, rt.v[1].world, rt.v[2].world,
                                  q.bary[0], q.bary[1], q.bary[2]);
                    normal = ref_normalize(bary3(rt.v[0].normal, rt.v[1].normal, rt.v[2].normal,
                                   q.bary[0], q.bary[1], q.bary[2]));
                    depth = (q.ndc.z + number(1)) / number(2);
                };

                int minx = int(std::floor(std::min(std::min(sa.x, sb.x), sc.x)));
                int maxx = int(std::ceil(std::max(std::max(sa.x, sb.x), sc.x)));
                int miny = int(std::floor(std::min(std::min(sa.y, sb.y), sc.y)));
                int maxy = int(std::ceil(std::max(std::max(sa.y, sb.y), sc.y)));
                if (minx < 0) minx = 0;
                if (miny < 0) miny = 0;
                if (maxx > int(N) - 1) maxx = int(N) - 1;
                if (maxy > int(N) - 1) maxy = int(N) - 1;

                for (int py = miny; py <= maxy; ++py) {
                    for (int px = minx; px <= maxx; ++px) {
                        const number cx = px + number(0.5);
                        const number cy = py + number(0.5);
                        auto edge = [](const V3 &a, const V3 &b, number x, number y) {
                            return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
                        };
                        number e0 = edge(sa, sb, cx, cy);
                        number e1 = edge(sb, sc, cx, cy);
                        number e2 = edge(sc, sa, cx, cy);
                        if (area > 0) {
                            if (e0 < 0 || e1 < 0 || e2 < 0) continue;
                        } else {
                            if (e0 > 0 || e1 > 0 || e2 > 0) continue;
                        }
                        const number aa = area > 0 ? area : -area;
                        number w0, w1, w2;
                        if (area > 0) {
                            w0 = e1 / aa; // opposite sa
                            w1 = e2 / aa; // opposite sb
                            w2 = e0 / aa; // opposite sc
                        } else {
                            w0 = -e1 / aa;
                            w1 = -e2 / aa;
                            w2 = -e0 / aa;
                        }
                        V3 wa, wb, wc, na, nb, nc;
                        number za, zb, zc;
                        attr(pa, wa, na, za);
                        attr(pb, wb, nb, zb);
                        attr(pc, wc, nc, zc);
                        V3 world = bary3(wa, wb, wc, w0, w1, w2);
                        V3 normal = ref_normalize(bary3(na, nb, nc, w0, w1, w2));
                        const number depth = za * w0 + zb * w1 + zc * w2;

                        ref_sample &cell = map[py * N + px];
                        if (cell.valid && depth > cell.depth) continue; // nearest wins
                        cell.valid = true;
                        cell.world = world;
                        cell.normal = normal;
                        cell.depth = depth;
                        cell.w0 = w0;
                        cell.w1 = w1;
                        cell.w2 = w2;
                        const number facing = rsm_math::dot3(normal, -ldir);
                        cell.front = facing > number(0);
                        if (cell.front) {
                            const number scale = texel_area / kPi;
                            cell.flux = rsm_rgb<number>{
                                    rt.reflectance.r * light.irradiance.r * scale,
                                    rt.reflectance.g * light.irradiance.g * scale,
                                    rt.reflectance.b * light.irradiance.b * scale};
                        } else {
                            cell.flux = rsm_rgb<number>{0, 0, 0};
                        }
                    }
                }
            }
        }
        return map;
    }

    rsm_light<number> make_basic_light(number aperture = 8) {        rsm_light<number> light;
        light.view.identity();
        light.projection = ortho(-aperture / 2, aperture / 2,
                                 -aperture / 2, aperture / 2,
                                 number(0.5), number(10));
        light.light_direction = V3{0, 0, -1};
        light.world_width = aperture;
        light.world_height = aperture;
        light.irradiance = rsm_rgb<number>{1, 1, 1};
        return light;
    }

    // Build a two-triangle square wall in local space, lying in the XY plane.
    void make_wall(std::vector<V3> &pos, std::vector<V3> &nrm,
                   std::vector<rsm_rgb<number> > &refl, number half, number z,
                   const rsm_rgb<number> &color, V3 normal = V3{0, 0, 1}) {
        pos = {{-half, -half, z}, {half, -half, z}, {half, half, z},
               {-half, -half, z}, {half, half, z}, {-half, half, z}};
        nrm.assign(6, normal);
        refl.assign(2, color);
    }

    std::vector<ref_triangle> make_ref_tris(
            const std::vector<V3> &world_pos, const std::vector<V3> &world_nrm,
            const std::vector<rsm_rgb<number> > &refl, const rsm_light<number> &light) {
        const M4 vp = light.projection * light.view;
        std::vector<ref_triangle> out;
        for (size_t t = 0; t < refl.size(); ++t) {
            ref_triangle rt;
            rt.reflectance = refl[t];
            for (int k = 0; k < 3; ++k) {
                const V3 &p = world_pos[t * 3 + k];
                rt.v[k].clip = vp * V4{p.x, p.y, p.z, 1};
                rt.v[k].world = p;
                rt.v[k].normal = world_nrm[t * 3 + k];
            }
            out.push_back(rt);
        }
        return out;
    }

    rsm_rgb<number> ref_transport(const std::vector<ref_sample> &map, unsigned N,
                                  const V3 &p, const V3 &n, number epsilon) {
        rsm_rgb<number> sum{0, 0, 0};
        for (unsigned i = 0; i < N * N; ++i) {
            const ref_sample &s = map[i];
            if (!s.valid || !s.front) continue;
            const V3 delta = p - s.world;
            const number r2 = rsm_math::dot3(delta, delta);
            const number r = number(std::sqrt(r2));
            if (!(r > epsilon)) continue;
            const V3 d = delta / r;
            number sf = rsm_math::dot3(s.normal, d);
            number rf = rsm_math::dot3(n, -d);
            if (sf < 0) sf = 0;
            if (rf < 0) rf = 0;
            if (sf == 0 || rf == 0) continue;
            const number wgt = sf * rf / r2;
            sum.r += s.flux.r * wgt;
            sum.g += s.flux.g * wgt;
            sum.b += s.flux.b * wgt;
        }
        return sum;
    }

    int count_valid(const Rsm<number> &rsm, unsigned N) {
        int c = 0;
        for (unsigned y = 0; y < N; ++y)
            for (unsigned x = 0; x < N; ++x)
                if (rsm.texelValid(int(x), int(y))) ++c;
        return c;
    }

    int count_ref_valid(const std::vector<ref_sample> &map) {
        int c = 0;
        for (size_t i = 0; i < map.size(); ++i) if (map[i].valid) ++c;
        return c;
    }

    // project a world point into continuous light-map screen coordinates.
    V3 light_screen(const rsm_light<number> &light, unsigned N, const V3 &p) {
        const M4 vp = light.projection * light.view;
        const V4 clip = vp * V4{p.x, p.y, p.z, 1};
        const V4 ndc = clip / clip.w;
        return V3{(ndc.x + 1) * number(N) / 2,
                  number(N) - (ndc.y + 1) * number(N) / 2,
                  (ndc.z + 1) / 2};
    }
}

// ============================================================================
// capture group
// ============================================================================
static int capture_basic_coverage_and_flux() {
    const unsigned N = 16;
    rsm_light<number> light = make_basic_light(8);
    std::vector<V3> pos, nrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(pos, nrm, refl, 3, -2, rsm_rgb<number>{1, 0, 0});

    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model.identity();

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surface, light, N));
    TASK_CHECK(rsm.mapSize() == N);
    TASK_CHECK(rsm.hasCapture());

    std::vector<ref_triangle> ref_tris = make_ref_tris(pos, nrm, refl, light);
    std::vector<ref_sample> ref = reference_capture(ref_tris, light, N);

    TASK_CHECK(count_valid(rsm, N) == count_ref_valid(ref));
    TASK_CHECK(count_valid(rsm, N) == 144);

    const number expected_area = number(64) / number(N * N);
    const number expected_flux = expected_area / kPi;
    int interior_matched = 0;
    for (unsigned y = 0; y < N; ++y) {
        for (unsigned x = 0; x < N; ++x) {
            const ref_sample &rs = ref[y * N + x];
            if (!rs.valid) continue;
            TASK_CHECK(rsm.texelValid(int(x), int(y)));
            // flux depends only on rho, E and the constant texel area, so it is
            // exact for every covered texel, including edge/fill-rule samples.
            rsm_rgb<number> flux;
            TASK_CHECK(rsm.texelFlux(int(x), int(y), flux));
            TASK_CHECK(near(flux.r, expected_flux, number(1e-5)));
            TASK_CHECK(near(flux.g, 0, number(1e-7)));
            TASK_CHECK(near(flux.b, 0, number(1e-7)));
            TASK_CHECK(near_vec(rsm.texelNormal(int(x), int(y)), rs.normal, number(1e-4)));

            // geometry is cross-checked away from edges and the internal triangle
            // seam, where the canvas integer barycentric quantization is bounded by
            // a fraction of one texel (one texel spans 0.5 world units here).
            const number margin = number(0.08);
            const bool interior = rs.w0 > margin && rs.w1 > margin && rs.w2 > margin;
            if (interior) {
                TASK_CHECK(near_vec(rsm.texelPosition(int(x), int(y)), rs.world,
                                   number(0.05)));
                ++interior_matched;
            }
        }
    }
    TASK_CHECK(interior_matched >= 40);
    return 0;
}

static int capture_resolution_area_weight() {
    rsm_light<number> light8 = make_basic_light(8);
    std::vector<V3> pos, nrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(pos, nrm, refl, 3, -2, rsm_rgb<number>{0.5f, 0.75f, 1});

    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model.identity();

    Rsm<number> r8, r32;
    TASK_CHECK(r8.capture(surface, light8, 8));
    rsm_light<number> light32 = make_basic_light(8);
    TASK_CHECK(r32.capture(surface, light32, 32));

    rsm_rgb<number> f8, f32;
    TASK_CHECK(r8.texelFlux(4, 4, f8));
    TASK_CHECK(r32.texelFlux(16, 16, f32));
    // A = aperture^2 / N^2, so 8x8 texels carry 16x the flux of 32x32 texels.
    TASK_CHECK(near(f8.r / f32.r, 16, number(1e-3)));
    TASK_CHECK(near(f8.g / f32.g, 16, number(1e-3)));
    TASK_CHECK(near(f8.b / f32.b, 16, number(1e-3)));

    // explicit rho*E*A/pi value at N=8.
    const number a8 = number(64) / number(64);
    TASK_CHECK(near(f8.r, number(0.5) * a8 / kPi, number(1e-5)));

    // legal square sizes span 8..64; out of range is rejected.
    Rsm<number> bad;
    TASK_CHECK(!bad.capture(surface, light8, 7));
    TASK_CHECK(!bad.capture(surface, light8, 65));
    TASK_CHECK(!bad.hasCapture());
    return 0;
}

static int capture_tilted_and_clipped() {
    // A sloped reflector whose top crosses the light near plane, forcing the
    // six-plane clipper to split it. One vertex sits behind the near plane.
    const unsigned N = 32;
    rsm_light<number> light = make_basic_light(8);
    std::vector<V3> pos = {
            {-3, -3, -3}, {3, -3, -3}, {3, 3, -0.2f},
            {-3, -3, -3}, {3, 3, -0.2f}, {-3, 3, -0.2f}};
    const V3 e1 = pos[1] - pos[0];
    const V3 e2 = pos[2] - pos[0];
    const V3 face = ref_normalize(rsm_math::cross3(e1, e2));
    std::vector<V3> nrm(6, face);
    std::vector<rsm_rgb<number> > refl(2, rsm_rgb<number>{0.8f, 0.8f, 0.8f});

    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model.identity();

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surface, light, N));
    TASK_CHECK(count_valid(rsm, N) > 0);

    std::vector<ref_triangle> ref_tris = make_ref_tris(pos, nrm, refl, light);
    std::vector<ref_sample> ref = reference_capture(ref_tris, light, N);
    TASK_CHECK(count_ref_valid(ref) > 0);
    // coverage can differ only on boundary texels (the canvas integer top-left
    // fill rule vs. the float reference's center test); the totals stay within a
    // narrow boundary band.
    const int prod_count = count_valid(rsm, N);
    const int ref_count = count_ref_valid(ref);
    const int count_diff = prod_count > ref_count ? prod_count - ref_count
                                                   : ref_count - prod_count;
    TASK_CHECK(count_diff <= 16);
    TASK_CHECK(prod_count > 400);

    // Geometry checks that are independent of the steep z slope:
    //  (a) every stored position reprojects back into its own texel (sub-texel);
    //  (b) every stored position lies exactly on the analytic surface plane;
    //  (c) a planar reflector yields a constant, exact normal at every texel.
    int matched = 0;
    for (unsigned y = 0; y < N; ++y) {
        for (unsigned x = 0; x < N; ++x) {
            if (!rsm.texelValid(int(x), int(y))) continue;
            const V3 wp = rsm.texelPosition(int(x), int(y));
            const V3 sp = light_screen(light, N, wp);
            TASK_CHECK(near(sp.x, x + number(0.5), number(0.55)));
            TASK_CHECK(near(sp.y, y + number(0.5), number(0.55)));
            TASK_CHECK(near(rsm_math::dot3(face, wp - pos[0]), number(0), number(1e-4)));
            TASK_CHECK(near_vec(rsm.texelNormal(int(x), int(y)), face, number(1e-3)));

            // away from the clip edge / seam, the float reference and the
            // production rasterizer also agree in full world coordinates.
            const ref_sample &rs = ref[y * N + x];
            const number margin = number(0.15);
            if (rs.valid && rs.w0 > margin && rs.w1 > margin && rs.w2 > margin) {
                TASK_CHECK(near_vec(wp, rs.world, number(0.15)));
                ++matched;
            }
        }
    }
    TASK_CHECK(matched > 20);
    // every stored position stays inside the light frustum after clipping.
    for (unsigned y = 0; y < N; ++y)
        for (unsigned x = 0; x < N; ++x)
            if (rsm.texelValid(int(x), int(y))) {
                const V3 p = rsm.texelPosition(int(x), int(y));
                TASK_CHECK(p.x >= -4.01f && p.x <= 4.01f);
                TASK_CHECK(p.y >= -4.01f && p.y <= 4.01f);
            }
    return 0;
}

static int capture_non_uniform_scale_normals() {
    const unsigned N = 32;
    rsm_light<number> light = make_basic_light(10);
    // local unit wall, model scales x by 2, y by 0.5; normals must use inverse transpose.
    std::vector<V3> lpos, lnrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(lpos, lnrm, refl, 1, -3, rsm_rgb<number>{1, 1, 1});
    // give the local normal a slant so inverse-transpose scaling is observable.
    for (auto &n : lnrm) n = ref_normalize(V3{1, 1, 0});

    M4 model = M4::scale(2, number(0.5), 1);
    rsm_surface<number> surface;
    surface.positions = lpos.data();
    surface.normals = lnrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model = model;

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surface, light, N));

    const M4 nmat = rsm_math::normal_matrix(model);
    const V3 expected_n = ref_normalize(rsm_math::transform_direction(nmat, lnrm[0]));

    // inverse of the orthographic aperture mapping (aperture 10, centered origin):
    // a texel center projects to a known world (x,y); z stays on the wall plane.
    auto texel_center_world = [&](int tx, int ty) {
        const number sx = tx + number(0.5);
        const number sy = ty + number(0.5);
        const number wx = (number(2) * sx / number(N) - number(1)) * number(5);
        const number wy = (number(1) - number(2) * sy / number(N)) * number(5);
        return V3{wx, wy, -3};
    };

    int checked = 0, interior_checked = 0;
    for (int ty = 0; ty < int(N); ++ty) {
        for (int tx = 0; tx < int(N); ++tx) {
            if (!rsm.texelValid(tx, ty)) continue;
            const V3 got = rsm.texelPosition(tx, ty);

            // every stored position reprojects back into its own texel.
            const V3 sp = light_screen(light, N, got);
            TASK_CHECK(near(sp.x, tx + number(0.5), number(0.55)));
            TASK_CHECK(near(sp.y, ty + number(0.5), number(0.55)));
            // z is fixed by the wall plane.
            TASK_CHECK(near(got.z, -3, number(1e-4)));
            // slanted inverse-transpose normal is identical at every texel.
            TASK_CHECK(near_vec(rsm.texelNormal(tx, ty), expected_n, number(1e-3)));
            ++checked;

            // deep interior texels (ideal point well inside the scaled wall
            // extent +/-2 in x, +/-0.5 in y) match the analytic position tightly.
            const V3 ideal = texel_center_world(tx, ty);
            if (ideal.x > number(-1.5) && ideal.x < number(1.5) &&
                ideal.y > number(-0.3) && ideal.y < number(0.3)) {
                // within ~1/6 of a texel (one texel spans 0.3125 world units).
                TASK_CHECK(near_vec(got, ideal, number(0.05)));
                ++interior_checked;
            }
        }
    }
    TASK_CHECK(checked >= 30);
    TASK_CHECK(interior_checked >= 8);

    // the inverse-transpose normal differs from a naive model transform: scaling
    // x by 2 and y by 0.5 makes the (1,1,0) normal lean toward the compressed axis.
    TASK_CHECK(std::fabs(expected_n.x - expected_n.y) > number(0.1));
    return 0;
}

static int capture_ignores_degenerate() {
    const unsigned N = 16;
    rsm_light<number> light = make_basic_light(8);
    std::vector<V3> pos = {
            {0, 0, -2}, {0, 0, -2}, {0, 0, -2},          // degenerate
            {-2, -2, -2}, {2, -2, -2}, {2, 2, -2},       // real tri
            {-2, -2, -2}, {2, 2, -2}, {-2, 2, -2}};      // real tri
    std::vector<V3> nrm(9, V3{0, 0, 1});
    std::vector<rsm_rgb<number> > refl = {{0, 0, 0}, {1, 1, 1}, {1, 1, 1}};

    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 3;
    surface.model.identity();

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surface, light, N));
    TASK_CHECK(count_valid(rsm, N) > 0);
    rsm_rgb<number> flux;
    TASK_CHECK(rsm.texelFlux(8, 8, flux));
    TASK_CHECK(flux.r > 0);
    return 0;
}

// ============================================================================
// transport group
// ============================================================================
static int transport_matches_discrete_reference() {
    const unsigned N = 16;
    rsm_light<number> light = make_basic_light(8);
    std::vector<V3> pos, nrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(pos, nrm, refl, 3, -2, rsm_rgb<number>{0.9f, 0.2f, 0.4f});

    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model.identity();

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surface, light, N));
    std::vector<ref_triangle> ref_tris = make_ref_tris(pos, nrm, refl, light);
    std::vector<ref_sample> ref = reference_capture(ref_tris, light, N);

    const V3 p{0.4f, -0.3f, -1};
    const V3 n{0, 0, -1};
    const number eps = 1e-4f;

    rsm_rgb<number> got = rsm.accumulate(p, n, eps);
    rsm_rgb<number> want = ref_transport(ref, N, p, n, eps);
    // discrete texel quantization gives a small relative difference; the two
    // implementations must agree within ~3%.
    TASK_CHECK(near_rel(got.r, want.r, number(0.03)));
    TASK_CHECK(near_rel(got.g, want.g, number(0.03)));
    TASK_CHECK(near_rel(got.b, want.b, number(0.03)));
    TASK_CHECK(got.r > 0);

    // shade = reflectance * (ambient + gain * sum / pi), still linear. Check the
    // formula against production's own sum so discretization cannot mask an error.
    rsm_rgb<number> recv{0.5f, 0.6f, 0.7f};
    rsm_rgb<number> shaded = rsm.shade(p, n, recv, number(0.1), number(2), eps);
    TASK_CHECK(near(shaded.r, recv.r * (number(0.1) + number(2) * got.r / kPi), number(1e-5)));
    TASK_CHECK(near(shaded.g, recv.g * (number(0.1) + number(2) * got.g / kPi), number(1e-5)));
    TASK_CHECK(near(shaded.b, recv.b * (number(0.1) + number(2) * got.b / kPi), number(1e-5)));
    return 0;
}

static int transport_epsilon_skips_near_sources() {
    const unsigned N = 16;
    rsm_light<number> light = make_basic_light(8);
    std::vector<V3> pos, nrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(pos, nrm, refl, 3, -2, rsm_rgb<number>{1, 1, 1});

    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model.identity();

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surface, light, N));

    // receiver offset in front of the reflector plane and facing the wall; a tiny
    // epsilon keeps the near sources while a large epsilon removes them all.
    const V3 on_plane = rsm.texelPosition(8, 8);
    const V3 p{on_plane.x, on_plane.y, number(-1.9)};
    const V3 n{0, 0, -1};
    rsm_rgb<number> small = rsm.accumulate(p, n, number(1e-6));
    rsm_rgb<number> big = rsm.accumulate(p, n, number(100));
    TASK_CHECK(small.r > 0);
    TASK_CHECK(big.r == 0 && big.g == 0 && big.b == 0);
    return 0;
}

static int transport_two_colored_walls_and_backface() {
    const unsigned N = 24;
    rsm_light<number> beam = make_basic_light(8);

    // red wall and blue wall at different depths; plus a back-facing wall whose
    // flux must be zero even though it occludes (double sided blocking).
    std::vector<V3> pos;
    std::vector<V3> nrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(pos, nrm, refl, 2, -3, rsm_rgb<number>{1, 0, 0});          // red, front

    std::vector<V3> bpos, bnrm;
    std::vector<rsm_rgb<number> > brefl;
    make_wall(bpos, bnrm, brefl, 2, -4, rsm_rgb<number>{0, 0, 1});       // blue, front

    // back-facing wall closer to the light, normal points away from the light.
    std::vector<V3> kpos, knrm;
    std::vector<rsm_rgb<number> > krefl;
    make_wall(kpos, knrm, krefl, 1, -1.5f, rsm_rgb<number>{0, 1, 0},
              V3{0, 0, -1}); // normal opposite the incoming light

    rsm_surface<number> surfaces[3];
    surfaces[0].positions = pos.data();
    surfaces[0].normals = nrm.data();
    surfaces[0].reflectance = refl.data();
    surfaces[0].triangle_count = 2;
    surfaces[0].model.identity();
    surfaces[1].positions = bpos.data();
    surfaces[1].normals = bnrm.data();
    surfaces[1].reflectance = brefl.data();
    surfaces[1].triangle_count = 2;
    surfaces[1].model.identity();
    surfaces[2].positions = kpos.data();
    surfaces[2].normals = knrm.data();
    surfaces[2].reflectance = krefl.data();
    surfaces[2].triangle_count = 2;
    surfaces[2].model.identity();

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surfaces, 3, beam, N));

    // the back-face region is a valid occluder but carries no flux.
    int back_valid = 0, back_zero = 0;
    for (unsigned y = 0; y < N; ++y)
        for (unsigned x = 0; x < N; ++x) {
            const V3 wp = rsm.texelPosition(int(x), int(y));
            if (!rsm.texelValid(int(x), int(y))) continue;
            if (near(wp.z, number(-1.5), number(0.01))) {
                ++back_valid;
                rsm_rgb<number> f;
                rsm.texelFlux(int(x), int(y), f);
                if (f.r == 0 && f.g == 0 && f.b == 0) ++back_zero;
            }
        }
    TASK_CHECK(back_valid > 0);
    TASK_CHECK(back_zero == back_valid);

    // receiver in front of the red wall collects red bleed.
    const V3 p{0, 0, -0.5f}, n{0, 0, -1};
    rsm_rgb<number> sum = rsm.accumulate(p, n, number(1e-4));
    TASK_CHECK(sum.r > 0);
    TASK_CHECK(sum.b >= 0);
    return 0;
}

static int transport_out_of_frustum_geometry_ignored() {
    const unsigned N = 16;
    rsm_light<number> light = make_basic_light(8);
    std::vector<V3> pos, nrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(pos, nrm, refl, 2, -2, rsm_rgb<number>{1, 1, 1});

    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model.identity();

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surface, light, N));
    const rsm_rgb<number> before = rsm.accumulate(V3{0, 0, -1}, V3{0, 0, -1}, number(1e-4));

    // a second snapshot adding geometry entirely outside the aperture must not
    // change the receiver result; out-of-map geometry contributes nothing.
    std::vector<V3> opos, onrm;
    std::vector<rsm_rgb<number> > orefl;
    make_wall(opos, onrm, orefl, 2, -2, rsm_rgb<number>{1, 0, 1});
    for (auto &pnt : opos) pnt.x += 50; // far outside the 8-wide aperture
    rsm_surface<number> outside;
    outside.positions = opos.data();
    outside.normals = onrm.data();
    outside.reflectance = orefl.data();
    outside.triangle_count = 2;
    outside.model.identity();
    rsm_surface<number> both[2] = {surface, outside};
    Rsm<number> rsm2;
    TASK_CHECK(rsm2.capture(both, 2, light, N));
    const rsm_rgb<number> after = rsm2.accumulate(V3{0, 0, -1}, V3{0, 0, -1}, number(1e-4));
    TASK_CHECK(near(before.r, after.r, number(1e-6)));
    TASK_CHECK(near(before.g, after.g, number(1e-6)));
    return 0;
}

// ============================================================================
// camera group
// ============================================================================
using Canvas24 = canvas<bitmap<coder::RGB888_PACKED_32>, CANVAS_OPT_64_BIT_FREE>;
using ReceiverShader = rsm_receiver_shader<number, microgl::rgba_t<8, 8, 8, 0> >;

static Rsm<number> make_camera_rsm() {
    rsm_light<number> light = make_basic_light(8);
    std::vector<V3> pos, nrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(pos, nrm, refl, 3, -3, rsm_rgb<number>{1, 0.3f, 0.2f});
    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model.identity();
    Rsm<number> rsm;
    rsm.capture(surface, light, 24);
    return rsm;
}

static int camera_perspective_receiver_triangle() {
    Rsm<number> rsm = make_camera_rsm();
    const int W = 48, H = 48;
    Canvas24 canvas(W, H);
    z_buffer<16> depth(W, H);
    canvas.clear({0, 0, 0, 255});

    ReceiverShader shader;
    shader.rsm = &rsm;
    shader.model.identity();
    shader.normal_matrix.identity();
    shader.receiver_reflectance = rsm_rgb<number>{0.8f, 0.8f, 0.8f};
    shader.ambient = number(0.05);
    shader.gain = number(1);
    shader.epsilon = number(1e-4);
    // camera at world z=+2.5 looking down -Z, so the receiver (z ~ -1.5..-2.2)
    // and the reflector wall (z=-3) are both in front of it.
    M4 view = translation(0, 0, number(-2.5));
    shader.camera_matrix = perspective(kPi / 3, 1, number(0.1), number(20)) * view;

    // unequal w: vertices at three different depths. The receiver sits in front of
    // the z=-3 reflector wall and faces it (normal -Z), so it collects its bleed.
    ReceiverShader::vertex_attributes a{{-2, -2, -1.5f}, {0, 0, -1}};
    ReceiverShader::vertex_attributes b{{2, -2, -1.8f}, {0, 0, -1}};
    ReceiverShader::vertex_attributes c{{0, 2, -2.2f}, {0, 0, -1}};
    canvas.drawTriangle<blendmode::Normal, porterduff::None<>,
            false, true, true>(shader, W, H, a, b, c, 255,
            microtess::triangles::face_culling::none, &depth);

    // independent perspective-correct world position at an interior pixel.
    auto world_at = [&](int px, int py, V3 &out_p, V3 &out_n) {
        const M4 m = shader.camera_matrix;
        V3 local[3] = {{-2, -2, -1.5f}, {2, -2, -1.8f}, {0, 2, -2.2f}};
        V4 clip[3];
        number inv_w[3];
        for (int k = 0; k < 3; ++k) {
            clip[k] = m * V4{local[k].x, local[k].y, local[k].z, 1};
            inv_w[k] = number(1) / clip[k].w;
        }
        // screen positions of the vertices (continuous canvas coords).
        auto scr = [&](const V4 &cl) {
            const V4 ndc = cl / cl.w;
            return V3{(ndc.x + 1) * W / 2, H - (ndc.y + 1) * H / 2, 0};
        };
        const V3 s[3] = {scr(clip[0]), scr(clip[1]), scr(clip[2])};
        const number area = (s[1].x - s[0].x) * (s[2].y - s[0].y) -
                            (s[1].y - s[0].y) * (s[2].x - s[0].x);
        const number cx = px + number(0.5), cy = py + number(0.5);
        auto edge = [&](int i, int j) {
            return (s[j].x - s[i].x) * (cy - s[i].y) -
                   (s[j].y - s[i].y) * (cx - s[i].x);
        };
        number e0 = edge(0, 1), e1 = edge(1, 2), e2 = edge(2, 0);
        number b0 = e1 / area, b1 = e2 / area, b2 = e0 / area;
        // perspective correct weights.
        number w0 = b0 * inv_w[0], w1 = b1 * inv_w[1], w2 = b2 * inv_w[2];
        const number wsum = w0 + w1 + w2;
        w0 /= wsum; w1 /= wsum; w2 /= wsum;
        out_p = V3{local[0].x * w0 + local[1].x * w1 + local[2].x * w2,
                   local[0].y * w0 + local[1].y * w1 + local[2].y * w2,
                   local[0].z * w0 + local[1].z * w1 + local[2].z * w2};
        out_n = V3{0, 0, -1};
    };

    int checked = 0;
    // scan a central region and compare only pixels the canvas actually covered
    // (the clear color is black; receiver colors here are well above it).
    for (int py = 10; py <= 40; ++py) {
        for (int px = 10; px <= 40; ++px) {
            color_t got;
            canvas.getPixelColor(px, py, got);
            if (got.r == 0 && got.g == 0 && got.b == 0) continue;

            V3 wp, wn;
            world_at(px, py, wp, wn);
            rsm_rgb<number> linear = rsm.shade(wp, wn, shader.receiver_reflectance,
                                               shader.ambient, shader.gain, shader.epsilon);
            auto to_byte = [](number v) {
                if (v < 0) v = 0;
                int q = int(v * 255 + number(0.5));
                if (q < 0) q = 0;
                if (q > 255) q = 255;
                return q;
            };
            // the only difference is the canvas's fixed-point perspective varying
            // interpolation, which stays within a couple of byte levels.
            TASK_CHECK(near(number(got.r), number(to_byte(linear.r)), number(3)));
            TASK_CHECK(near(number(got.g), number(to_byte(linear.g)), number(3)));
            TASK_CHECK(near(number(got.b), number(to_byte(linear.b)), number(3)));
            // the bleed is visibly tinted and above the ambient floor.
            TASK_CHECK(got.r > got.b);
            ++checked;
        }
    }
    TASK_CHECK(checked >= 30);
    return 0;
}

static int camera_occlusion_and_depth() {
    Rsm<number> rsm = make_camera_rsm();
    const int W = 32, H = 32;
    Canvas24 canvas(W, H);
    z_buffer<16> depth(W, H);
    canvas.clear({0, 0, 0, 255});

    ReceiverShader far_shader, near_shader;
    far_shader.rsm = &rsm;
    near_shader.rsm = &rsm;
    far_shader.model.identity();
    near_shader.model.identity();
    far_shader.normal_matrix.identity();
    near_shader.normal_matrix.identity();
    far_shader.receiver_reflectance = rsm_rgb<number>{1, 1, 1};
    near_shader.receiver_reflectance = rsm_rgb<number>{0, 0, 0}; // black, ambient 0
    far_shader.ambient = number(0.2);
    near_shader.ambient = number(0);
    far_shader.gain = number(1);
    near_shader.gain = number(0);
    far_shader.epsilon = near_shader.epsilon = number(1e-4);
    M4 view = translation(0, 0, 0);
    far_shader.camera_matrix = perspective(kPi / 3, 1, 0.1, 20) * view;
    near_shader.camera_matrix = far_shader.camera_matrix;

    using Attr = ReceiverShader::vertex_attributes;
    Attr f0{{-2, -2, -6}, {0, 0, 1}}, f1{{2, -2, -6}, {0, 0, 1}},
         f2{{0, 2, -6}, {0, 0, 1}};
    Attr q0{{-2, -2, -3}, {0, 0, 1}}, q1{{2, -2, -3}, {0, 0, 1}},
         q2{{0, 2, -3}, {0, 0, 1}};

    canvas.drawTriangle<blendmode::Normal, porterduff::None<>,
            false, true, true>(far_shader, W, H, f0, f1, f2, 255,
            microtess::triangles::face_culling::none, &depth);
    canvas.drawTriangle<blendmode::Normal, porterduff::None<>,
            false, true, true>(near_shader, W, H, q0, q1, q2, 255,
            microtess::triangles::face_culling::none, &depth);

    color_t center;
    canvas.getPixelColor(16, 16, center);
    // near black triangle wins over the bright far triangle in the overlap.
    TASK_CHECK(center.r < 10 && center.g < 10 && center.b < 10);
    // the depth buffer now stores the nearer surface.
    TASK_CHECK(depth[16 * W + 16] != depth.max_value);
    return 0;
}

static int camera_uses_own_depth_and_keeps_rsm_readonly() {
    Rsm<number> rsm = make_camera_rsm();
    const unsigned N = rsm.mapSize();
    rsm_rgb<number> before_flux;
    rsm.texelFlux(10, 10, before_flux);
    const V3 before_pos = rsm.texelPosition(10, 10);

    const int W = 32, H = 32;
    Canvas24 canvas(W, H);
    z_buffer<16> depth(W, H);
    canvas.clear({10, 20, 30, 255});

    ReceiverShader shader;
    shader.rsm = &rsm;
    shader.model.identity();
    shader.normal_matrix.identity();
    shader.receiver_reflectance = rsm_rgb<number>{0.5f, 0.5f, 0.5f};
    shader.ambient = number(0.1);
    shader.gain = number(1);
    shader.epsilon = number(1e-4);
    shader.camera_matrix = perspective(kPi / 3, 1, 0.1, 20) * translation(0, 0, 0);

    using Attr = ReceiverShader::vertex_attributes;
    Attr a{{-2, -2, -4}, {0, 0, 1}}, b{{2, -2, -4}, {0, 0, 1}},
         c{{0, 2, -4}, {0, 0, 1}};
    canvas.drawTriangle<blendmode::Normal, porterduff::None<>,
            false, true, true>(shader, W, H, a, b, c, 255,
            microtess::triangles::face_culling::none, &depth);

    // camera updated its own depth buffer ...
    bool depth_touched = false;
    for (int i = 0; i < W * H; ++i)
        if (depth[i] != depth.max_value) { depth_touched = true; break; }
    TASK_CHECK(depth_touched);
    // ... while the captured RSM is byte-for-byte unchanged.
    rsm_rgb<number> after_flux;
    rsm.texelFlux(10, 10, after_flux);
    TASK_CHECK(near(before_flux.r, after_flux.r, number(0)));
    TASK_CHECK(near_vec(before_pos, rsm.texelPosition(10, 10), number(0)));
    TASK_CHECK(rsm.mapSize() == N);

    // an RSM-disabled shader still keeps the original color_shader interface/output.
    using CS = color_shader<number, microgl::rgba_t<8, 8, 8, 0> >;
    Canvas24 plain(W, H);
    z_buffer<16> plain_depth(W, H);
    plain.clear({0, 0, 0, 255});
    CS cs;
    cs.matrix.identity();
    CS::vertex_attributes ca{{-1, -1, 0}, {200, 10, 10, 255}};
    CS::vertex_attributes cb{{1, -1, 0}, {200, 10, 10, 255}};
    CS::vertex_attributes cc{{0, 1, 0}, {200, 10, 10, 255}};
    plain.drawTriangle<blendmode::Normal, porterduff::None<>,
            false, true, true>(cs, W, H, ca, cb, cc, 255,
            microtess::triangles::face_culling::none, &plain_depth);
    color_t pc;
    plain.getPixelColor(16, 16, pc);
    TASK_CHECK(pc.r >= 190 && pc.g < 30 && pc.b < 30);
    return 0;
}

// ============================================================================
// lifetime group
// ============================================================================
static int lifetime_snapshot_owns_its_data() {
    const unsigned N = 16;
    rsm_light<number> light = make_basic_light(8);
    std::vector<V3> pos, nrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(pos, nrm, refl, 3, -2, rsm_rgb<number>{1, 0.5f, 0.25f});

    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model.identity();

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surface, light, N));
    rsm_rgb<number> saved_flux;
    rsm.texelFlux(8, 8, saved_flux);
    const V3 saved_pos = rsm.texelPosition(8, 8);
    const V3 saved_n = rsm.texelNormal(8, 8);

    // mutate every input array after capture.
    for (auto &p : pos) p.x += 10;
    for (auto &n : nrm) n = V3{1, 0, 0};
    for (auto &r : refl) r = rsm_rgb<number>{0, 1, 0};

    // reuse an unrelated canvas; the snapshot must not reference any of it.
    Canvas24 scratch(8, 8);
    scratch.clear({123, 45, 67, 255});

    rsm_rgb<number> now_flux;
    rsm.texelFlux(8, 8, now_flux);
    TASK_CHECK(near(saved_flux.r, now_flux.r, number(0)));
    TASK_CHECK(near(saved_flux.g, now_flux.g, number(0)));
    TASK_CHECK(near_vec(saved_pos, rsm.texelPosition(8, 8), number(0)));
    TASK_CHECK(near_vec(saved_n, rsm.texelNormal(8, 8), number(0)));
    return 0;
}

static int lifetime_recapture_replaces_and_failure_keeps_old() {
    rsm_light<number> light = make_basic_light(8);
    std::vector<V3> pos, nrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(pos, nrm, refl, 3, -2, rsm_rgb<number>{1, 0, 0});
    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model.identity();

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surface, light, 16));
    rsm_rgb<number> first_flux;
    rsm.texelFlux(8, 8, first_flux);
    TASK_CHECK(first_flux.r > 0 && first_flux.g == 0);

    // recapture with a green reflector at a different resolution.
    for (auto &r : refl) r = rsm_rgb<number>{0, 1, 0};
    TASK_CHECK(rsm.capture(surface, light, 32));
    TASK_CHECK(rsm.mapSize() == 32);
    rsm_rgb<number> second_flux;
    TASK_CHECK(rsm.texelFlux(16, 16, second_flux));
    TASK_CHECK(second_flux.g > 0 && second_flux.r == 0);

    // a failing capture leaves the green snapshot intact.
    TASK_CHECK(!rsm.capture(surface, light, 4)); // size out of range
    TASK_CHECK(rsm.mapSize() == 32);
    rsm_rgb<number> still_green;
    TASK_CHECK(rsm.texelFlux(16, 16, still_green));
    TASK_CHECK(near(still_green.g, second_flux.g, number(0)));

    rsm_surface<number> null_surface;
    null_surface.positions = nullptr;
    null_surface.normals = nullptr;
    null_surface.reflectance = nullptr;
    null_surface.triangle_count = 2;
    null_surface.model.identity();
    TASK_CHECK(!rsm.capture(null_surface, light, 16));
    TASK_CHECK(rsm.mapSize() == 32);
    return 0;
}

static int lifetime_two_snapshots_interleaved() {
    rsm_light<number> light = make_basic_light(8);

    std::vector<V3> rpos, rnrm;
    std::vector<rsm_rgb<number> > rrefl;
    make_wall(rpos, rnrm, rrefl, 2, -2, rsm_rgb<number>{1, 0, 0});
    rsm_surface<number> red;
    red.positions = rpos.data();
    red.normals = rnrm.data();
    red.reflectance = rrefl.data();
    red.triangle_count = 2;
    red.model.identity();

    std::vector<V3> bpos, bnrm;
    std::vector<rsm_rgb<number> > brefl;
    make_wall(bpos, bnrm, brefl, 2, -2, rsm_rgb<number>{0, 0, 1});
    rsm_surface<number> blue;
    blue.positions = bpos.data();
    blue.normals = bnrm.data();
    blue.reflectance = brefl.data();
    blue.triangle_count = 2;
    blue.model.identity();

    Rsm<number> rsm_red, rsm_blue;
    TASK_CHECK(rsm_red.capture(red, light, 16));
    TASK_CHECK(rsm_blue.capture(blue, light, 16));

    // interleave reads; the two snapshots never alias.
    for (int iter = 0; iter < 3; ++iter) {
        rsm_rgb<number> rf, bf;
        TASK_CHECK(rsm_red.texelFlux(8, 8, rf));
        TASK_CHECK(rsm_blue.texelFlux(8, 8, bf));
        TASK_CHECK(rf.r > 0 && rf.b == 0);
        TASK_CHECK(bf.b > 0 && bf.r == 0);
        TASK_CHECK(near_vec(rsm_red.texelPosition(8, 8),
                            rsm_blue.texelPosition(8, 8), number(1e-5)));
    }
    return 0;
}

static int lifetime_capture_does_not_touch_camera() {
    const int W = 24, H = 24;
    Canvas24 canvas(W, H);
    z_buffer<16> depth(W, H);
    canvas.clear({7, 8, 9, 255});
    depth.fill(1234);

    rsm_light<number> light = make_basic_light(8);
    std::vector<V3> pos, nrm;
    std::vector<rsm_rgb<number> > refl;
    make_wall(pos, nrm, refl, 3, -2, rsm_rgb<number>{1, 1, 1});
    rsm_surface<number> surface;
    surface.positions = pos.data();
    surface.normals = nrm.data();
    surface.reflectance = refl.data();
    surface.triangle_count = 2;
    surface.model.identity();

    Rsm<number> rsm;
    TASK_CHECK(rsm.capture(surface, light, 16));

    for (int i = 0; i < W * H; ++i) TASK_CHECK(depth[i] == 1234);
    color_t px;
    canvas.getPixelColor(5, 5, px);
    TASK_CHECK(px.r == 7 && px.g == 8 && px.b == 9);
    return 0;
}

// ============================================================================
// registration
// ============================================================================
extern const struct task_case task_cases[] = {
        {"capture", "basic_coverage_and_flux", capture_basic_coverage_and_flux},
        {"capture", "resolution_area_weight", capture_resolution_area_weight},
        {"capture", "tilted_and_near_clipped", capture_tilted_and_clipped},
        {"capture", "non_uniform_scale_normals", capture_non_uniform_scale_normals},
        {"capture", "degenerate_triangle_ignored", capture_ignores_degenerate},

        {"transport", "matches_discrete_reference", transport_matches_discrete_reference},
        {"transport", "epsilon_skips_near_sources", transport_epsilon_skips_near_sources},
        {"transport", "two_colored_walls_and_backface", transport_two_colored_walls_and_backface},
        {"transport", "out_of_frustum_geometry_ignored", transport_out_of_frustum_geometry_ignored},

        {"camera", "perspective_receiver_triangle", camera_perspective_receiver_triangle},
        {"camera", "occlusion_and_depth", camera_occlusion_and_depth},
        {"camera", "own_depth_rsm_readonly_and_plain_shader", camera_uses_own_depth_and_keeps_rsm_readonly},

        {"lifetime", "snapshot_owns_its_data", lifetime_snapshot_owns_its_data},
        {"lifetime", "recapture_replaces_failure_keeps_old", lifetime_recapture_replaces_and_failure_keeps_old},
        {"lifetime", "two_snapshots_interleaved", lifetime_two_snapshots_interleaved},
        {"lifetime", "capture_does_not_touch_camera", lifetime_capture_does_not_touch_camera},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
