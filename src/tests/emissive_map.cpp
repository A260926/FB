// tests/emissive_map.cpp*
//
// Tests for BilinearAreaLight: a triangle area light whose emission follows a
// low-resolution bilinear RGB map in texture space.
//
// The groups registered here are:
//   EmissiveMapGeometry  -- emission evaluation, UV mapping, normals and
//                           transforms (including reflections and non-uniform
//                           scaling).
//   EmissiveMapDensity   -- the area sampling density pA = Y(E)/Z, exact
//                           integrals, black regions, and sampling frequency.
//   EmissiveMapTransport -- conversion between area and solid-angle
//                           densities, forward/reverse PDFs, time, two-sided
//                           emission, occlusion, and a full
//                           GeometricPrimitive/Scene/EstimateDirect check.
//
// Independent references are computed by clipping each map grid cell against
// the texture-space triangle and integrating the bilinear field exactly over
// the resulting polygon (see RefIntegral... below); this does not share the
// slab/column decomposition used internally by the light.

#include "task_support/contract.h"

#include "tests/gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pbrt.h"
#include "api.h"
#include "paramset.h"
#include "interaction.h"
#include "lights/bilinear.h"
#include "lights/diffuse.h"
#include "shapes/triangle.h"
#include "shapes/disk.h"
#include "primitive.h"
#include "scene.h"
#include "sampling.h"
#include "lowdiscrepancy.h"
#include "memory.h"
#include "reflection.h"
#include "materials/matte.h"
#include "textures/constant.h"
#include "accelerators/bvh.h"
#include "samplers/zerotwosequence.h"
#include "core/integrator.h"

using namespace pbrt;

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::shared_ptr<Shape> MakeTriangle(const Transform &o2w, const Point3f *P,
                                    const int *indices, int nVerts,
                                    const Point2f *uv, bool reverse = false) {
    Transform w2o = Inverse(o2w);
    std::vector<std::shared_ptr<Shape>> shapes = CreateTriangleMesh(
        &o2w, &w2o, reverse, 1, indices, nVerts, P, nullptr, nullptr, uv,
        nullptr, nullptr);
    EXPECT_EQ(1u, shapes.size());
    return shapes.empty() ? nullptr : shapes[0];
}

// One triangle from three world-space points and optional UVs.
std::shared_ptr<Shape> MakeSimpleTriangle(const Point3f P[3], const Point2f *uv,
                                          bool reverse = false) {
    static const Transform id;
    int indices[3] = {0, 1, 2};
    return MakeTriangle(id, P, indices, 3, uv, reverse);
}

std::vector<Spectrum> MakeMap(int nu, int nv,
                              const std::function<Spectrum(int, int)> &f) {
    std::vector<Spectrum> m((size_t)nu * nv);
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) m[(size_t)j * nu + i] = f(i, j);
    return m;
}

std::vector<Spectrum> MakeRGBMap(int nu, int nv,
                                 const std::function<void(int, int, Float *)> &f) {
    std::vector<Spectrum> m((size_t)nu * nv);
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            Float rgb[3];
            f(i, j, rgb);
            m[(size_t)j * nu + i] = Spectrum::FromRGB(rgb);
        }
    return m;
}

std::shared_ptr<BilinearAreaLight> MakeLight(
    const Spectrum &L, const std::shared_ptr<Shape> &shape, int nu, int nv,
    const std::vector<Spectrum> &map, bool twoSided = false,
    const Transform &lightToWorld = Transform()) {
    return std::make_shared<BilinearAreaLight>(lightToWorld, MediumInterface(),
                                               L, 1, shape, nu, nv, map,
                                               twoSided);
}

Float RelError(Float a, Float b) {
    Float denom = std::max(std::abs(a), std::abs(b));
    if (denom == 0) return 0;
    return std::abs(a - b) / denom;
}

// ---------------------------------------------------------------------------
// Independent exact reference integrals.
//
// The emissive field is piecewise bilinear on the map grid.  For a reference
// value we clip every grid cell rectangle against the texture-space triangle
// (Sutherland-Hodgman), then integrate the cell's bilinear polynomial exactly
// over the resulting convex polygon.  The polygon is fan-triangulated about
// its first vertex, and the monomial moments of each triangle are evaluated in
// closed form.
// ---------------------------------------------------------------------------

typedef std::vector<Point2f> Poly;

Poly ClipHalfPlane(const Poly &in, double a, double b, double c) {
    // Keep points with a*u + b*v + c >= 0.
    Poly out;
    const size_t n = in.size();
    if (n == 0) return out;
    for (size_t i = 0; i < n; ++i) {
        Point2f A = in[i], B = in[(i + 1) % n];
        double fa = a * A.x + b * A.y + c;
        double fb = a * B.x + b * B.y + c;
        bool inA = fa >= 0, inB = fb >= 0;
        if (inA) out.push_back(A);
        if (inA != inB) {
            double t = fa / (fa - fb);
            out.push_back(Point2f((Float)(A.x + t * (B.x - A.x)),
                                  (Float)(A.y + t * (B.y - A.y))));
        }
    }
    return out;
}

double SignedArea2(const Poly &p) {
    double s = 0;
    for (size_t i = 0; i < p.size(); ++i)
        s += (double)p[i].x * p[(i + 1) % p.size()].y -
             (double)p[(i + 1) % p.size()].x * p[i].y;
    return s;
}

struct Moments {
    double area = 0, iu = 0, iv = 0, iuv = 0;
};

// Add the moments of triangle (A,B,C) to m.
void AddTriangleMoments(const Point2f &A, const Point2f &B, const Point2f &C,
                        Moments *m) {
    double area = 0.5 * ((double)(B.x - A.x) * (C.y - A.y) -
                         (double)(C.x - A.x) * (B.y - A.y));
    if (area <= 0) return;
    double su = (double)A.x + B.x + C.x;
    double sv = (double)A.y + B.y + C.y;
    m->area += area;
    m->iu += area * su / 3.0;
    m->iv += area * sv / 3.0;
    m->iuv += area / 12.0 * (su * sv + (double)A.x * A.y +
                            (double)B.x * B.y + (double)C.x * C.y);
}

Moments PolygonMoments(Poly p) {
    Moments m;
    if (p.size() < 3) return m;
    if (SignedArea2(p) < 0) std::reverse(p.begin(), p.end());
    for (size_t i = 1; i + 1 < p.size(); ++i)
        AddTriangleMoments(p[0], p[i], p[i + 1], &m);
    return m;
}

// Integrate a cell's bilinear field (global coefficients) over a polygon.
double IntegralPoly(const Moments &m, double a, double b, double c, double d) {
    return a * m.area + b * m.iu + c * m.iv + d * m.iuv;
}

// Clip a rectangle [u0,u1]x[v0,v1] to the texture-space triangle and return
// the polygon (possibly empty).
Poly CellInsideTriangle(const Point2f uv[3], double u0, double u1, double v0,
                        double v1) {
    Poly tri = {{uv[0].x, uv[0].y}, {uv[1].x, uv[1].y}, {uv[2].x, uv[2].y}};
    if (SignedArea2(tri) < 0) std::reverse(tri.begin(), tri.end());
    Poly p = tri;
    p = ClipHalfPlane(p, 1, 0, -u0);
    p = ClipHalfPlane(p, -1, 0, u1);
    p = ClipHalfPlane(p, 0, 1, -v0);
    p = ClipHalfPlane(p, 0, -1, v1);
    return p;
}

// Global bilinear coefficients for a cell, given the four corner values.
void CellCoefficients(double f00, double f10, double f01, double f11, double u0,
                      double u1, double v0, double v1, double *a, double *b,
                      double *c, double *d) {
    double bu = (f10 - f00) / (u1 - u0);
    double cv = (f01 - f00) / (v1 - v0);
    double m = (f00 - f10 - f01 + f11) / ((u1 - u0) * (v1 - v0));
    *d = m;
    *b = bu - m * v0;
    *c = cv - m * u0;
    *a = f00 - bu * u0 - cv * v0 + m * u0 * v0;
}

// Exact integral over the texture-space triangle of the bilinear field whose
// grid samples are `values` (one scalar per grid point), for a single channel.
double RefIntegralUV(const Point2f uv[3], int nu, int nv,
                     const std::vector<double> &values) {
    const double du = 1.0 / (nu - 1), dv = 1.0 / (nv - 1);
    double total = 0;
    for (int j = 0; j < nv - 1; ++j) {
        for (int i = 0; i < nu - 1; ++i) {
            double u0 = i * du, u1 = (i + 1) * du;
            double v0 = j * dv, v1 = (j + 1) * dv;
            Poly poly = CellInsideTriangle(uv, u0, u1, v0, v1);
            if (poly.size() < 3) continue;
            Moments m = PolygonMoments(poly);
            if (!(m.area > 0)) continue;
            double f00 = values[(size_t)j * nu + i];
            double f10 = values[(size_t)j * nu + i + 1];
            double f01 = values[(size_t)(j + 1) * nu + i];
            double f11 = values[(size_t)(j + 1) * nu + i + 1];
            double a, b, c, d;
            CellCoefficients(f00, f10, f01, f11, u0, u1, v0, v1, &a, &b, &c,
                             &d);
            total += IntegralPoly(m, a, b, c, d);
        }
    }
    return total;
}

double RefIntegralUVChannel(const Point2f uv[3], int nu, int nv,
                            const std::vector<Spectrum> &map, int channel,
                            Float L = 1) {
    std::vector<double> v(map.size());
    for (size_t i = 0; i < map.size(); ++i) v[i] = (double)map[i][channel] * L;
    return RefIntegralUV(uv, nu, nv, v);
}

// Texture-space area of a triangle.
double UVArea(const Point2f uv[3]) {
    return 0.5 * std::abs((double)(uv[1].x - uv[0].x) * (uv[2].y - uv[0].y) -
                         (double)(uv[2].x - uv[0].x) * (uv[1].y - uv[0].y));
}

// Map a sample index to a fixed 2D quasi-random point in [0,1)^2.
Point2f FixedSample(uint64_t i, int dim) {
    return Point2f(RadicalInverse(dim + 0, i), RadicalInverse(dim + 1, i));
}

// Texture-space triangle used by many tests, deliberately oblique so it
// crosses several grid cells and is not aligned to the map axes.
void ObliqueUVs(Point2f uv[3]) {
    uv[0] = Point2f(0.08f, 0.12f);
    uv[1] = Point2f(0.93f, 0.21f);
    uv[2] = Point2f(0.24f, 0.88f);
}

void ObliqueWorld(Point3f P[3]) {
    P[0] = Point3f(0, 0, 0);
    P[1] = Point3f(3, 0, 1);
    P[2] = Point3f(0, 4, 2);
}

}  // namespace

// ===========================================================================
// EmissiveMapGeometry
// ===========================================================================

// Bilinear interpolation inside cells, exact grid-point values, and the
// requested map[j*nu+i] indexing, on an oblique UV triangle crossing multiple
// grid cells.
TEST(EmissiveMapGeometry, ObliqueGridValues) {
    const int nu = 5, nv = 4;
    std::vector<Spectrum> map = MakeMap(nu, nv, [](int i, int j) {
        return Spectrum(0.25f + 0.5f * i + 3.0f * j + 0.75f * i * j);
    });
    Point3f P[3];
    Point2f uv[3];
    ObliqueWorld(P);
    ObliqueUVs(uv);
    auto shape = MakeSimpleTriangle(P, uv);
    ASSERT_NE(nullptr, shape);
    const Float L = 2.5f;
    auto light = MakeLight(Spectrum(L), shape, nu, nv, map);
    TASK_CHECK(light->IsValid());

    // Grid-point values: E(i/(nu-1), j/(nv-1)) = L * map[j*nu+i].
    int bad = 0;
    for (int j = 0; j < nv; ++j) {
        for (int i = 0; i < nu; ++i) {
            Point2f g((Float)i / (nu - 1), (Float)j / (nv - 1));
            Spectrum expected = map[(size_t)j * nu + i] * L;
            Spectrum got = light->RadianceAtUV(g);
            for (int c = 0; c < 3; ++c)
                if (RelError(expected[c], got[c]) > 2e-6) ++bad;
        }
    }
    TASK_CHECK(bad == 0);

    // Interior interpolation: at the centre of cell (1,2), the bilinear
    // formula must match the direct average of the four cell corners.
    Point2f cellCenter(1.5f / (nu - 1), 2.5f / (nv - 1));
    Spectrum expect = 0.25f * (map[(size_t)2 * nu + 1] + map[(size_t)2 * nu + 2] +
                               map[(size_t)3 * nu + 1] + map[(size_t)3 * nu + 2]) *
                      L;
    Spectrum got = light->RadianceAtUV(cellCenter);
    for (int c = 0; c < 3; ++c)
        TASK_CHECK(RelError(expect[c], got[c]) < 2e-6);

    // Power is pi * integral of E over the triangle; compare every channel
    // against the independent polygon-clipped reference.
    for (int c = 0; c < 3; ++c) {
        double refInt = RefIntegralUVChannel(uv, nu, nv, map, c, L);
        double refPower = Pi * refInt * (shape->Area() / UVArea(uv));
        TASK_CHECK(RelError((Float)refPower, light->Power()[c]) < 2e-5);
    }
}

// When no UVs are supplied, the triangle uses the built-in default mapping
// (0,0), (1,0), (1,1); emission must follow it.
TEST(EmissiveMapGeometry, DefaultUVs) {
    const int nu = 3, nv = 3;
    Point3f P[3] = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
    std::vector<Spectrum> map = MakeMap(nu, nv, [](int i, int j) {
        return Spectrum(1.0f + 2.0f * i + 5.0f * j);
    });
    auto shape = MakeSimpleTriangle(P, nullptr);
    ASSERT_NE(nullptr, shape);
    auto light = MakeLight(Spectrum(1), shape, nu, nv, map);
    TASK_CHECK(light->IsValid());

    // Vertex 1 of the default mapping is UV (1,0), vertex 2 is (1,1).
    Point2f uvDefault[3] = {{0, 0}, {1, 0}, {1, 1}};
    TASK_CHECK(RelError(light->RadianceAtUV(uvDefault[0])[0], map[0][0]) < 2e-6);
    TASK_CHECK(RelError(light->RadianceAtUV(uvDefault[1])[0],
                        map[(size_t)0 * nu + 2][0]) < 2e-6);
    TASK_CHECK(RelError(light->RadianceAtUV(uvDefault[2])[0],
                        map[(size_t)2 * nu + 2][0]) < 2e-6);

    for (int c = 0; c < 3; ++c) {
        double refInt = RefIntegralUVChannel(uvDefault, nu, nv, map, c);
        double refPower = Pi * refInt * (2.0 / UVArea(uvDefault));
        TASK_CHECK(RelError((Float)refPower, light->Power()[c]) < 2e-5);
    }
}

// Reflection (negative scale, which swaps handedness) and a non-uniform,
// sheared transform: emission is evaluated through the recovered UVs, the
// geometric normal is the world-vertex cross product oriented by
// reverseOrientation XOR transformSwapsHandedness, and area/power scale
// correctly.
TEST(EmissiveMapGeometry, ReflectionAndNonUniformTransform) {
    const int nu = 4, nv = 4;
    std::vector<Spectrum> map = MakeMap(nu, nv, [](int i, int j) {
        return Spectrum(0.3f + 0.7f * i + 1.1f * j + 0.4f * i * j);
    });
    // Object-space triangle with the default-ish UV mapping.
    Point3f Pobj[3] = {{0, 0, 0}, {1.2f, 0, 0}, {0, 0.9f, 0.35f}};
    Point2f uv[3] = {{0.05f, 0.1f}, {0.95f, 0.2f}, {0.3f, 0.85f}};

    struct Case {
        Transform T;
        bool reverse;
    } cases[2] = {
        // Non-uniform scale plus shear.
        {Transform(Matrix4x4(2.0f, 0.4f, 0.0f, 1.0f, 0.0f, 0.5f, 0.2f, -2.0f,
                             0.1f, 0.0f, 1.5f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f)),
         false},
        // Reflection about x (swaps handedness).
        {Scale(-1.5f, 2.0f, 1.0f), false},
    };

    for (int k = 0; k < 2; ++k) {
        const Transform &T = cases[k].T;
        int indices[3] = {0, 1, 2};
        auto shape = MakeTriangle(T, Pobj, indices, 3, uv, cases[k].reverse);
        ASSERT_NE(nullptr, shape);
        auto light = MakeLight(Spectrum(1.5f), shape, nu, nv, map);
        TASK_CHECK(light->IsValid());

        // World vertices and expected normal.
        Point3f W[3];
        for (int i = 0; i < 3; ++i) W[i] = T(Pobj[i]);
        Vector3f ng = Cross(W[1] - W[0], W[2] - W[0]);
        TASK_CHECK(ng.LengthSquared() > 0);
        Normal3f expectN = Normal3f(Normalize(ng));
        bool swap = T.SwapsHandedness();
        if (cases[k].reverse ^ swap) expectN *= -1;
        Normal3f gotN = light->GeometricNormalValue();
        TASK_CHECK(RelError(gotN.x, expectN.x) < 1e-6);
        TASK_CHECK(RelError(gotN.y, expectN.y) < 1e-6);
        TASK_CHECK(RelError(gotN.z, expectN.z) < 1e-6);

        // World area matches the transformed triangle.
        double expectArea =
            0.5 * (double)Cross(W[1] - W[0], W[2] - W[0]).Length();
        TASK_CHECK(RelError((Float)expectArea, light->WorldArea()) < 1e-5);

        // Emission at a UV point is independent of the transform.
        Point2f uvq(0.4f, 0.55f);
        Spectrum expect = map[0];  // overwritten below
        {
            int i = (int)std::floor(uvq.x * (nu - 1));
            int j = (int)std::floor(uvq.y * (nv - 1));
            i = std::min(std::max(i, 0), nu - 2);
            j = std::min(std::max(j, 0), nv - 2);
            Float tx = uvq.x * (nu - 1) - i, ty = uvq.y * (nv - 1) - j;
            expect = (1 - tx) * (1 - ty) * map[(size_t)j * nu + i] +
                     tx * (1 - ty) * map[(size_t)j * nu + i + 1] +
                     (1 - tx) * ty * map[(size_t)(j + 1) * nu + i] +
                     tx * ty * map[(size_t)(j + 1) * nu + i + 1];
            expect *= 1.5f;
        }
        Point3f pos;
        Vector3f pError;
        light->UVToWorld(uvq, &pos, &pError);
        // Emission evaluated at that world position, on the emitting side.
        Vector3f w((Float)gotN.x, (Float)gotN.y, (Float)gotN.z);
        Interaction intr(pos, gotN, pError, -w, 0, MediumInterface());
        Spectrum got = light->L(intr, w);
        for (int c = 0; c < 3; ++c)
            TASK_CHECK(RelError(expect[c], got[c]) < 1e-5);

        // Power matches the world-area reference integral.
        double uvA = UVArea(uv);
        double jac = expectArea / uvA;
        for (int ch = 0; ch < 3; ++ch) {
            double refInt = RefIntegralUVChannel(uv, nu, nv, map, ch, 1.5f);
            double refPower = Pi * refInt * jac;
            TASK_CHECK(RelError((Float)refPower, light->Power()[ch]) < 2e-5);
        }
    }
}

// Ordinary Interaction L and a real ray/shape intersection must agree.
TEST(EmissiveMapGeometry, InteractionAndRealIntersection) {
    const int nu = 4, nv = 3;
    Point3f P[3] = {{0, 0, 0}, {2.5f, 0, 0}, {0, 2, 0}};
    Point2f uv[3] = {{0.1f, 0.2f}, {0.9f, 0.15f}, {0.35f, 0.8f}};
    std::vector<Spectrum> map = MakeMap(nu, nv, [](int i, int j) {
        return Spectrum(0.5f + 1.3f * i + 0.9f * j);
    });
    auto shape = MakeSimpleTriangle(P, uv);
    ASSERT_NE(nullptr, shape);
    auto light = MakeLight(Spectrum(1), shape, nu, nv, map);
    TASK_CHECK(light->IsValid());

    // Choose a texture coordinate, convert to world, and shoot a ray at it.
    Point2f uvq(0.42f, 0.51f);
    Point3f target;
    Vector3f pError;
    light->UVToWorld(uvq, &target, &pError);
    Spectrum expected = light->RadianceAtUV(uvq);

    // A generic Interaction (not a SurfaceInteraction) carrying only p.
    Interaction generic(target, Vector3f(0, 0, 1), 0.25f, MediumInterface());
    Vector3f w(0, 0, 1);
    Spectrum gotGeneric = light->L(generic, w);
    for (int c = 0; c < 3; ++c)
        TASK_CHECK(RelError(expected[c], gotGeneric[c]) < 2e-5);

    // Actual ray intersection through the Triangle.
    Ray ray(Point3f(target.x, target.y, 2.0f), Vector3f(0, 0, -1));
    Float tHit;
    SurfaceInteraction isect;
    TASK_CHECK(shape->Intersect(ray, &tHit, &isect, false));
    Spectrum gotHit = light->L(isect, -ray.d);
    for (int c = 0; c < 3; ++c)
        TASK_CHECK(RelError(expected[c], gotHit[c]) < 2e-5);

    // The back side is dark for a one-sided light.
    Vector3f backW(0, 0, -1);
    TASK_CHECK(light->L(generic, backW).IsBlack());
}

// CreateBilinearAreaLight must return null (after reporting an Error) for an
// unsupported shape or invalid parameters, rather than silently falling back
// to a constant emitter.  Error() writes to stderr, which the test does not
// attempt to capture; here we check the returned pointer and that the process
// keeps running.
TEST(EmissiveMapGeometry, FactoryValidation) {
    // Build a ParamSet carrying nu, nv and a map, adjusting one field per
    // check.  AddRGBSpectrum expects flat RGB values.
    auto buildParams = [](int nu, int nv, int nRGBValues) {
        ParamSet ps;
        {
            std::unique_ptr<int[]> v(new int[1]);
            v[0] = nu;
            ps.AddInt("nu", std::move(v), 1);
        }
        {
            std::unique_ptr<int[]> v(new int[1]);
            v[0] = nv;
            ps.AddInt("nv", std::move(v), 1);
        }
        {
            std::unique_ptr<Float[]> v(new Float[nRGBValues]);
            for (int i = 0; i < nRGBValues; ++i) v[i] = 1.f;
            ps.AddRGBSpectrum("map", std::move(v), nRGBValues);
        }
        return ps;
    };

    Point3f P[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    Point2f uv[3] = {{0, 0}, {1, 0}, {0, 1}};
    auto tri = MakeSimpleTriangle(P, uv);
    ASSERT_NE(nullptr, tri);

    Transform id;
    MediumInterface mi;

    // Valid parameters produce a light.
    {
        ParamSet ps = buildParams(2, 2, 12);  // 4 points * 3 channels
        auto light = CreateBilinearAreaLight(id, nullptr, ps, tri);
        TASK_CHECK(light != nullptr);
    }

    // nu < 2.
    {
        ParamSet ps = buildParams(1, 2, 6);
        auto light = CreateBilinearAreaLight(id, nullptr, ps, tri);
        TASK_CHECK(light == nullptr);
    }
    // Map length does not match nu*nv.
    {
        ParamSet ps = buildParams(2, 2, 9);  // 3 points instead of 4
        auto light = CreateBilinearAreaLight(id, nullptr, ps, tri);
        TASK_CHECK(light == nullptr);
    }
    // Missing "map".
    {
        ParamSet ps;
        {
            std::unique_ptr<int[]> a(new int[1]);
            a[0] = 2;
            ps.AddInt("nu", std::move(a), 1);
            std::unique_ptr<int[]> b(new int[1]);
            b[0] = 2;
            ps.AddInt("nv", std::move(b), 1);
        }
        auto light = CreateBilinearAreaLight(id, nullptr, ps, tri);
        TASK_CHECK(light == nullptr);
    }
    // Degenerate (zero-area) triangle.
    {
        Point3f D[3] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
        auto deg = MakeSimpleTriangle(D, uv);
        ASSERT_NE(nullptr, deg);
        ParamSet ps = buildParams(2, 2, 12);
        auto light = CreateBilinearAreaLight(id, nullptr, ps, deg);
        TASK_CHECK(light == nullptr);
    }
    // UVs outside [0,1]^2.
    {
        Point2f badUV[3] = {{0, 0}, {1.5f, 0}, {0, 1}};
        auto bad = MakeSimpleTriangle(P, badUV);
        ASSERT_NE(nullptr, bad);
        ParamSet ps = buildParams(2, 2, 12);
        auto light = CreateBilinearAreaLight(id, nullptr, ps, bad);
        TASK_CHECK(light == nullptr);
    }
    // Degenerate UV triangle.
    {
        Point2f badUV[3] = {{0, 0}, {0.5f, 0.5f}, {1, 1}};
        auto bad = MakeSimpleTriangle(P, badUV);
        ASSERT_NE(nullptr, bad);
        ParamSet ps = buildParams(2, 2, 12);
        auto light = CreateBilinearAreaLight(id, nullptr, ps, bad);
        TASK_CHECK(light == nullptr);
    }
    // Unsupported (non-triangle) shape: use a disk.
    {
        Transform d2w = Translate(Vector3f(0, 0, -1));
        Transform w2d = Inverse(d2w);
        std::shared_ptr<Shape> disk(
            new Disk(&d2w, &w2d, false, 0.0f, 1.0f, 0.0f, 360.0f));
        ParamSet ps = buildParams(2, 2, 12);
        auto light = CreateBilinearAreaLight(id, nullptr, ps, disk);
        TASK_CHECK(light == nullptr);
    }
    // Negative colour value in the map.
    {
        ParamSet ps;
        {
            std::unique_ptr<int[]> a(new int[1]);
            a[0] = 2;
            ps.AddInt("nu", std::move(a), 1);
            std::unique_ptr<int[]> b(new int[1]);
            b[0] = 2;
            ps.AddInt("nv", std::move(b), 1);
        }
        {
            std::unique_ptr<Float[]> v(new Float[12]);
            for (int i = 0; i < 12; ++i) v[i] = 1.f;
            v[4] = -0.5f;
            ps.AddRGBSpectrum("map", std::move(v), 12);
        }
        auto light = CreateBilinearAreaLight(id, nullptr, ps, tri);
        TASK_CHECK(light == nullptr);
    }
}

// ===========================================================================
// EmissiveMapDensity
// ===========================================================================

// Hand-computable case: on the reference triangle, a globally affine field
// E(u,v) = 1 + u + v (obtained from an affine map) has Z = A*(1+1/3+1/3) and
// E_pA[u] = E_pA[v] = 0.35.  The density itself is
// pA(u,v) = (1+u+v)/Z * (world area / uv area), and it must agree at points.
TEST(EmissiveMapDensity, AffineFieldMoments) {
    // A 2x2 bilinear map is affine when f00+f11=f10+f01; use E = 1+u+v.
    const int nu = 2, nv = 2;
    std::vector<Spectrum> map = MakeMap(nu, nv, [](int i, int j) {
        return Spectrum(1.0f + i + j);  // (0,0)=1 (1,0)=2 (0,1)=2 (1,1)=3
    });
    Point3f P[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    Point2f uv[3] = {{0, 0}, {1, 0}, {0, 1}};
    auto shape = MakeSimpleTriangle(P, uv);
    ASSERT_NE(nullptr, shape);
    auto light = MakeLight(Spectrum(1), shape, nu, nv, map);
    TASK_CHECK(light->IsValid());

    // uvArea = 0.5, worldArea = 0.5 -> jacobian 1.
    const double Zuv = 5.0 / 6.0;
    // The area density integrates to one over the triangle.
    TASK_CHECK(std::abs(shape->Area() - 0.5) < 1e-6);

    // Power = pi * integral E dA = pi * (5/6).
    TASK_CHECK(RelError((Float)(Pi * 5.0 / 6.0), light->Power()[0]) < 2e-5);

    // Pointwise density.
    for (int k = 0; k < 5; ++k) {
        double u = 0.1 + 0.15 * k, v = 0.05 + 0.12 * k;
        if (u + v >= 1) continue;
        double expect = (1.0 + u + v) / Zuv;  // world-area density, jac=1
        TASK_CHECK(RelError((Float)expect, light->AreaPdfUV(Point2f((Float)u, (Float)v))) < 2e-5);
    }

    // The density's normalizer must equal the independent exact integral of
    // Y(E) over the triangle.  Recover Z from pA = Y/Z at a few points and
    // compare against the polygon-clipped reference.
    {
        std::vector<double> ygrid(map.size());
        for (size_t i = 0; i < map.size(); ++i) ygrid[i] = (double)map[i].y();
        double jac = shape->Area() / 0.5;
        double Zref = RefIntegralUV(uv, nu, nv, ygrid) * jac;
        int bad = 0;
        for (int k = 0; k < 4; ++k) {
            double uu = 0.12 + 0.19 * k, vv = 0.09 + 0.17 * k;
            if (uu + vv >= 1) continue;
            Float pA = light->AreaPdfUV(Point2f((Float)uu, (Float)vv));
            Float Y = light->YAtUV(Point2f((Float)uu, (Float)vv));
            TASK_CHECK(pA > 0);
            double Zimplied = (double)Y / pA;
            if (RelError((Float)Zref, (Float)Zimplied) > 2e-5) ++bad;
        }
        printf("EmissiveMapDensity.AffineFieldMoments: Zref=%.9f\n", Zref);
        TASK_CHECK(bad == 0);
    }

    // Sampling frequency: E_pA[u] = E_pA[v] = 0.35 (exact hand calculation).
    const int N = 250000;
    double sumU = 0, sumV = 0;
    for (int i = 0; i < N; ++i) {
        Ray ray;
        Normal3f n;
        Float pdfPos, pdfDir;
        light->Sample_Le(FixedSample(i, 0), Point2f(0.37f, 0.61f), 0.5f, &ray,
                         &n, &pdfPos, &pdfDir);
        Point2f s;
        TASK_CHECK(light->WorldToUV(ray.o, &s));
        sumU += s.x;
        sumV += s.y;
    }
    double meanU = sumU / N, meanV = sumV / N;
    // With fixed quasi-random samples the observed error is well under 1e-3;
    // print both the sample count and the deviation so the bound is auditable.
    double errU = std::abs(meanU - 0.35), errV = std::abs(meanV - 0.35);
    printf("EmissiveMapDensity.AffineFieldMoments: N=%d meanU=%.6f meanV=%.6f "
           "errU=%.3g errV=%.3g (hand reference 0.35)\n",
           N, meanU, meanV, errU, errV);
    TASK_CHECK(errU < 2e-3);
    TASK_CHECK(errV < 2e-3);
}

// RGB power on a non-trivial field: every channel must match the independent
// polygon-clipped integral, and the field must not be treated as a grey map.
TEST(EmissiveMapDensity, RGBPowerAndBlackRegion) {
    const int nu = 3, nv = 3;
    // Cell (0,0) is entirely black: corners (0,0),(1,0),(0,1),(1,1) all zero.
    std::vector<Spectrum> map = MakeRGBMap(nu, nv, [](int i, int j, Float *rgb) {
        bool black = (i <= 1 && j <= 1);
        rgb[0] = black ? 0.f : 0.5f + 2.0f * i + 0.3f * j;
        rgb[1] = black ? 0.f : 0.2f + 0.4f * i + 1.7f * j;
        rgb[2] = black ? 0.f : 0.9f + 1.1f * i * j + 0.25f * j;
    });
    Point3f P[3] = {{0, 0, 0}, {2, 0, 0}, {0, 1.5f, 0}};
    Point2f uv[3] = {{0, 0}, {1, 0}, {0, 1}};
    auto shape = MakeSimpleTriangle(P, uv);
    ASSERT_NE(nullptr, shape);
    auto light = MakeLight(Spectrum(1), shape, nu, nv, map);
    TASK_CHECK(light->IsValid());

    double jac = shape->Area() / UVArea(uv);
    for (int c = 0; c < 3; ++c) {
        double refInt = RefIntegralUVChannel(uv, nu, nv, map, c);
        double refPower = Pi * refInt * jac;
        TASK_CHECK(RelError((Float)refPower, light->Power()[c]) < 2e-5);
    }
    // RGB channels are genuinely different.
    TASK_CHECK(RelError(light->Power()[0], light->Power()[1]) > 0.05f);

    // The black sub-cell inside the triangle must have zero area density.
    TASK_CHECK(light->AreaPdfUV(Point2f(0.12f, 0.12f)) == 0.f);
    TASK_CHECK(light->AreaPdfUV(Point2f(0.4f, 0.05f)) == 0.f);  // u in [0,0.5), v<0.5
    TASK_CHECK(light->AreaPdfUV(Point2f(0.7f, 0.15f)) > 0.f);
    TASK_CHECK(light->AreaPdfUV(Point2f(0.2f, 0.7f)) > 0.f);

    // No position samples may land in the black sub-cell.
    const int N = 200000;
    int blackHits = 0;
    for (int i = 0; i < N; ++i) {
        Ray ray;
        Normal3f n;
        Float pdfPos, pdfDir;
        light->Sample_Le(FixedSample(i, 2), Point2f(0.5f, 0.5f), 0.5f, &ray, &n,
                         &pdfPos, &pdfDir);
        if (ray.o.x < 0.5 * 2.0f && ray.o.y < 0.5 * 1.5f) {
            // Within the UV-square cell (0,0) in world coordinates (the
            // triangle mapping here is axis aligned).
            Point2f s;
            if (light->WorldToUV(ray.o, &s) && s.x < 0.5f && s.y < 0.5f)
                ++blackHits;
        }
    }
    printf("EmissiveMapDensity.RGBPowerAndBlackRegion: N=%d blackHits=%d\n", N,
           blackHits);
    TASK_CHECK(blackHits == 0);
}

// A fully black triangle: zero power, no NaN, and a uniform-area fallback
// position distribution.
TEST(EmissiveMapDensity, AllBlackFallback) {
    const int nu = 3, nv = 2;
    Point3f P[3] = {{0, 0, 0}, {2.5f, 0, 0}, {0, 1.25f, 0}};
    Point2f uv[3] = {{0, 0}, {1, 0}, {0.25f, 0.9f}};
    std::vector<Spectrum> map((size_t)nu * nv, Spectrum(0.f));
    auto shape = MakeSimpleTriangle(P, uv);
    ASSERT_NE(nullptr, shape);
    auto light = MakeLight(Spectrum(3), shape, nu, nv, map);
    TASK_CHECK(light->IsValid());
    TASK_CHECK(light->Power().IsBlack());
    TASK_CHECK(std::isfinite(light->Power()[0]));

    const Float uniformPdf = 1.0f / shape->Area();
    for (int i = 0; i < 2000; ++i) {
        Ray ray;
        Normal3f n;
        Float pdfPos, pdfDir;
        Spectrum Le =
            light->Sample_Le(FixedSample(i, 3), Point2f(0.3f, 0.8f), 0.5f, &ray,
                             &n, &pdfPos, &pdfDir);
        TASK_CHECK(Le.IsBlack());
        TASK_CHECK(RelError(uniformPdf, pdfPos) < 2e-6);
        TASK_CHECK(std::isfinite(ray.o.x) && std::isfinite(ray.o.y) &&
                   std::isfinite(ray.o.z));
        TASK_CHECK(std::isfinite(pdfDir));
    }
    // Every UV in the triangle gets the same fallback density.
    TASK_CHECK(RelError(uniformPdf, light->AreaPdfUV(Point2f(0.2f, 0.3f))) < 2e-6);
    TASK_CHECK(RelError(uniformPdf, light->AreaPdfUV(Point2f(0.05f, 0.7f))) < 2e-6);

    // Sampling still returns the correct position density in solid angle.
    Interaction ref(Point3f(0.5f, 0.5f, 3.0f), 0, MediumInterface());
    Vector3f wi;
    Float pdf;
    VisibilityTester vis;
    Spectrum Li = light->Sample_Li(ref, Point2f(0.3f, 0.8f), &wi, &pdf, &vis);
    TASK_CHECK(Li.IsBlack());
    TASK_CHECK(std::isfinite(pdf) && pdf >= 0);
}

// Sampling frequency against an independent exact bin-mass reference on an
// oblique triangle crossing several cells.  This is a statistical check with a
// fixed sample sequence: the sample count and the observed L1 bin error are
// printed so the bound can be audited.
TEST(EmissiveMapDensity, SamplingFrequency) {
    const int nu = 4, nv = 4;
    std::vector<Spectrum> map = MakeMap(nu, nv, [](int i, int j) {
        return Spectrum(0.4f + 1.6f * i + 0.8f * j + 2.5f * i * j);
    });
    Point3f P[3];
    Point2f uv[3];
    ObliqueWorld(P);
    ObliqueUVs(uv);
    auto shape = MakeSimpleTriangle(P, uv);
    ASSERT_NE(nullptr, shape);
    auto light = MakeLight(Spectrum(1), shape, nu, nv, map);
    TASK_CHECK(light->IsValid());

    // Exact mass in each of an 8x8 grid of UV bins, computed by integrating
    // the field over the intersection of the bin and the triangle.  The
    // normalization uses the exact full-triangle field integral.
    const int G = 8;
    std::vector<double> ygrid(map.size());
    for (size_t i = 0; i < map.size(); ++i) ygrid[i] = (double)map[i].y();
    double Zuv = RefIntegralUV(uv, nu, nv, ygrid);
    TASK_CHECK(Zuv > 0);
    std::vector<double> exact(G * G, 0.0);
    std::vector<int> occupied;
    for (int by = 0; by < G; ++by) {
        for (int bx = 0; bx < G; ++bx) {
            double u0 = (double)bx / G, u1 = (double)(bx + 1) / G;
            double v0 = (double)by / G, v1 = (double)(by + 1) / G;
            // Clip the triangle to the bin rectangle, then integrate the
            // bilinear field over the polygon cell by cell.
            Poly tri = {{uv[0].x, uv[0].y},
                        {uv[1].x, uv[1].y},
                        {uv[2].x, uv[2].y}};
            if (SignedArea2(tri) < 0) std::reverse(tri.begin(), tri.end());
            Poly p = tri;
            p = ClipHalfPlane(p, 1, 0, -u0);
            p = ClipHalfPlane(p, -1, 0, u1);
            p = ClipHalfPlane(p, 0, 1, -v0);
            p = ClipHalfPlane(p, 0, -1, v1);
            if (p.size() < 3) continue;
            // Sum contributions from each map cell that overlaps the bin.
            double du = 1.0 / (nu - 1), dv = 1.0 / (nv - 1);
            double mass = 0;
            for (int j = 0; j < nv - 1; ++j) {
                for (int i = 0; i < nu - 1; ++i) {
                    double cu0 = i * du, cu1 = (i + 1) * du;
                    double cv0 = j * dv, cv1 = (j + 1) * dv;
                    // Intersect the already-clipped bin polygon with the cell.
                    Poly q = p;
                    q = ClipHalfPlane(q, 1, 0, -cu0);
                    q = ClipHalfPlane(q, -1, 0, cu1);
                    q = ClipHalfPlane(q, 0, 1, -cv0);
                    q = ClipHalfPlane(q, 0, -1, cv1);
                    if (q.size() < 3) continue;
                    Moments m = PolygonMoments(q);
                    if (!(m.area > 0)) continue;
                    double a, b, c, d;
                    CellCoefficients(ygrid[(size_t)j * nu + i],
                                     ygrid[(size_t)j * nu + i + 1],
                                     ygrid[(size_t)(j + 1) * nu + i],
                                     ygrid[(size_t)(j + 1) * nu + i + 1], cu0,
                                     cu1, cv0, cv1, &a, &b, &c, &d);
                    mass += IntegralPoly(m, a, b, c, d);
                }
            }
            if (mass > 1e-12) {
                exact[by * G + bx] = mass / Zuv;
                occupied.push_back(by * G + bx);
            }
        }
    }

    const int N = 1000000;
    std::vector<double> freq(G * G, 0.0);
    for (int i = 0; i < N; ++i) {
        Ray ray;
        Normal3f n;
        Float pdfPos, pdfDir;
        light->Sample_Le(FixedSample(i, 4), Point2f(0.5f, 0.5f), 0.5f, &ray, &n,
                         &pdfPos, &pdfDir);
        Point2f s;
        if (!light->WorldToUV(ray.o, &s)) continue;
        int bx = std::min(std::max((int)(s.x * G), 0), G - 1);
        int by = std::min(std::max((int)(s.y * G), 0), G - 1);
        freq[by * G + bx] += 1.0 / N;
    }
    double l1 = 0, maxRel = 0;
    for (int b : occupied) {
        double e = exact[b], f = freq[b];
        l1 += std::abs(f - e);
        if (e > 1e-3) maxRel = std::max(maxRel, std::abs(f - e) / e);
    }
    double totalFreq = 0, totalExact = 0;
    for (int b = 0; b < G * G; ++b) {
        totalFreq += freq[b];
        totalExact += exact[b];
    }
    printf("EmissiveMapDensity.SamplingFrequency: N=%d bins=%d L1=%.5f "
           "maxRel(%.1e bins)=%.5f totalFreq=%.6f totalExact=%.6f\n",
           N, (int)occupied.size(), l1, 1e-3, maxRel, totalFreq, totalExact);
    TASK_CHECK(std::abs(totalFreq - 1.0) < 2e-4);
    TASK_CHECK(std::abs(totalExact - 1.0) < 2e-4);
    // A wrong density would show L1 near O(1); with this fixed sequence the
    // observed L1 is far below the bound below.
    TASK_CHECK(l1 < 0.2);
    TASK_CHECK(maxRel < 0.12);
}

// ===========================================================================
// EmissiveMapTransport
// ===========================================================================

// Area vs solid-angle density, and matching forward/reverse position PDFs.
TEST(EmissiveMapTransport, DensityConversionAndPdfs) {
    const int nu = 4, nv = 4;
    std::vector<Spectrum> map = MakeMap(nu, nv, [](int i, int j) {
        return Spectrum(0.3f + 2.0f * i + 0.7f * j + 0.9f * i * j);
    });
    Point3f P[3] = {{0, 0, 0}, {2.5f, 0, 0}, {0, 1.8f, 0}};
    Point2f uv[3] = {{0.1f, 0.1f}, {0.9f, 0.2f}, {0.3f, 0.85f}};
    auto shape = MakeSimpleTriangle(P, uv);
    ASSERT_NE(nullptr, shape);
    auto light = MakeLight(Spectrum(1), shape, nu, nv, map);
    TASK_CHECK(light->IsValid());

    Interaction ref(Point3f(0.4f, 0.3f, 2.2f), Normal3f(), Vector3f(),
                    Vector3f(0, 0, 1), 0.37f, MediumInterface());

    // Sample_Li: pdf_solidAngle == pA(position) * r^2 / cos.
    int checked = 0;
    for (int i = 0; i < 4000; ++i) {
        Vector3f wi;
        Float pdf;
        VisibilityTester vis;
        Spectrum Li =
            light->Sample_Li(ref, FixedSample(i, 0), &wi, &pdf, &vis);
        if (pdf <= 0) continue;
        // Recover the light point along the returned direction.
        Ray ray = ref.SpawnRay(wi);
        Float tHit;
        SurfaceInteraction isectLight;
        if (!shape->Intersect(ray, &tHit, &isectLight, false)) continue;
        Float cos = AbsDot(light->GeometricNormalValue(), -wi);
        Float expect = light->AreaPdfUV(isectLight.uv) *
                       DistanceSquared(ref.p, isectLight.p) / cos;
        TASK_CHECK(RelError(expect, pdf) < 2e-5);
        // Pdf_Li agrees for directions that hit the triangle.
        Float pdfLi = light->Pdf_Li(ref, wi);
        TASK_CHECK(RelError(pdf, pdfLi) < 2e-5);
        ++checked;
    }
    TASK_CHECK(checked > 100);

    // Pdf_Li is zero for directions that miss the triangle.
    TASK_CHECK(light->Pdf_Li(ref, Vector3f(0, 0, 1)) == 0.f);
    TASK_CHECK(light->Pdf_Li(ref, Vector3f(1, 0, 0)) == 0.f);

    // Reverse PDFs: Sample_Le -> Pdf_Le, including the offset-ray origin and
    // the separately reported position/direction densities.
    for (int i = 0; i < 4000; ++i) {
        Ray ray;
        Normal3f nLight;
        Float pdfPos, pdfDir;
        Spectrum Le = light->Sample_Le(FixedSample(i, 2), FixedSample(i, 4),
                                       0.61f, &ray, &nLight, &pdfPos,
                                       &pdfDir);
        TASK_CHECK(ray.time == 0.61f);
        if (pdfPos <= 0) continue;
        Float backPos, backDir;
        light->Pdf_Le(ray, nLight, &backPos, &backDir);
        TASK_CHECK(RelError(pdfPos, backPos) < 2e-5);
        TASK_CHECK(RelError(pdfDir, backDir) < 2e-5);
        // The position density must vary with position on a non-uniform map.
        TASK_CHECK(pdfPos > 0);
        // Le must equal the map radiance at the ray origin's UV.
        Point2f s;
        TASK_CHECK(light->WorldToUV(ray.o, &s));
        Spectrum expectLe = light->RadianceAtUV(s);
        for (int c = 0; c < 3; ++c)
            TASK_CHECK(RelError(expectLe[c], Le[c]) < 2e-5);
    }

    // The position density is not constant: two widely separated positions
    // with different map values have different pA.
    Float pdfA = light->AreaPdfUV(Point2f(0.15f, 0.15f));
    Float pdfB = light->AreaPdfUV(Point2f(0.8f, 0.25f));
    TASK_CHECK(RelError(pdfA, pdfB) > 0.1f);
}

// Two-sided emission, back-face single-sided behaviour, and occlusion.
TEST(EmissiveMapTransport, TwoSidedAndOcclusion) {
    const int nu = 3, nv = 3;
    std::vector<Spectrum> map = MakeMap(nu, nv, [](int i, int j) {
        return Spectrum(0.8f + 1.0f * i + 0.5f * j + 0.6f * i * j);
    });
    Point3f P[3] = {{0, 0, 1}, {2, 0, 1}, {0, 2, 1}};
    Point2f uv[3] = {{0, 0}, {1, 0}, {0, 1}};
    auto shape = MakeSimpleTriangle(P, uv);
    ASSERT_NE(nullptr, shape);

    auto one = MakeLight(Spectrum(1), shape, nu, nv, map, /*twoSided=*/false);
    auto two = MakeLight(Spectrum(1), shape, nu, nv, map, /*twoSided=*/true);
    TASK_CHECK(one->IsValid() && two->IsValid());

    Interaction refBack(Point3f(0.4f, 0.4f, -1.5f), Normal3f(), Vector3f(),
                        Vector3f(0, 0, 1), 0.0f, MediumInterface());
    Interaction refFront(Point3f(0.4f, 0.4f, 3.5f), Normal3f(), Vector3f(),
                         Vector3f(0, 0, -1), 0.0f, MediumInterface());

    // A one-sided light samples positions and returns the position density
    // from behind, but emits black.
    Vector3f wi;
    Float pdf;
    VisibilityTester vis;
    Spectrum backLi =
        one->Sample_Li(refBack, Point2f(0.3f, 0.6f), &wi, &pdf, &vis);
    TASK_CHECK(pdf > 0);
    TASK_CHECK(backLi.IsBlack());
    // The two-sided light emits from the back side.
    Spectrum backTwo =
        two->Sample_Li(refBack, Point2f(0.3f, 0.6f), &wi, &pdf, &vis);
    TASK_CHECK(pdf > 0);
    TASK_CHECK(!backTwo.IsBlack());
    // Front side emits in both cases.
    Spectrum frontOne =
        one->Sample_Li(refFront, Point2f(0.3f, 0.6f), &wi, &pdf, &vis);
    TASK_CHECK(!frontOne.IsBlack());

    // Sample_Le on a one-sided light always emits into the hemisphere of its
    // geometric normal, so its radiance is never black and the direction
    // density is non-zero.
    Ray ray;
    Normal3f nLight;
    Float pdfPos, pdfDir;
    bool sawUp = false;
    for (int i = 0; i < 64; ++i) {
        Spectrum le = one->Sample_Le(Point2f(0.3f, 0.6f), FixedSample(i, 6),
                                     0.0f, &ray, &nLight, &pdfPos, &pdfDir);
        TASK_CHECK(pdfPos > 0);
        TASK_CHECK(pdfDir > 0);
        TASK_CHECK(!le.IsBlack());
        if (ray.d.z > 0 && le[0] > 0) sawUp = true;
    }
    TASK_CHECK(sawUp);

    // The two-sided light can emit into the lower hemisphere as well, still
    // with non-black radiance and a valid direction density.
    bool sawTwoSidedBack = false;
    for (int i = 0; i < 256; ++i) {
        Spectrum le = two->Sample_Le(Point2f(0.3f, 0.6f), FixedSample(i, 6),
                                     0.0f, &ray, &nLight, &pdfPos, &pdfDir);
        if (!le.IsBlack() && ray.d.z < 0 && pdfDir > 0) sawTwoSidedBack = true;
    }
    TASK_CHECK(sawTwoSidedBack);

    // Pdf_Le on the back side (relative to the one-sided normal): the position
    // density is still positive, but the direction density is zero.
    {
        Ray ray;
        Normal3f nLight;
        Float lp, ld;
        two->Sample_Le(Point2f(0.3f, 0.6f), Point2f(0.75f, 0.5f), 0.0f, &ray,
                       &nLight, &lp, &ld);
        TASK_CHECK(ray.d.z < 0);  // sampled into the lower hemisphere
        // Use the two-sided light's own normal (same geometry) but pretend
        // the light is one-sided for the direction-density query.
        Float bp, bd;
        one->Pdf_Le(ray, nLight, &bp, &bd);
        TASK_CHECK(bp > 0);
        TASK_CHECK(bd == 0.f);
        Float tp, td;
        two->Pdf_Le(ray, nLight, &tp, &td);
        TASK_CHECK(tp > 0);
        TASK_CHECK(td > 0);
    }
    // A ray origin off the triangle yields zero position density.
    Ray missRay(Point3f(5, 5, 5), Vector3f(0, 0, -1));
    Float bp, bd;
    one->Pdf_Le(missRay, Normal3f(0, 0, 1), &bp, &bd);
    TASK_CHECK(bp == 0.f);
    TASK_CHECK(bd == 0.f);

    // Occlusion: put a blocker between a receiver and the light and verify
    // VisibilityTester reports it.
    Transform id;
    Point3f Q[3] = {{-2, -2, 2.5f}, {4, -2, 2.5f}, {-2, 4, 2.5f}};
    int qidx[3] = {0, 1, 2};
    auto blocker = MakeTriangle(id, Q, qidx, 3, nullptr);
    std::vector<std::shared_ptr<Primitive>> prims;
    prims.push_back(std::make_shared<GeometricPrimitive>(
        blocker, nullptr, nullptr, MediumInterface()));
    prims.push_back(std::make_shared<GeometricPrimitive>(
        shape, nullptr, one, MediumInterface()));
    auto bvh = std::make_shared<BVHAccel>(prims);
    std::vector<std::shared_ptr<Light>> lights = {one};
    Scene scene(bvh, lights);

    Interaction recvFront(Point3f(0.4f, 0.4f, 4.0f), Normal3f(), Vector3f(),
                          Vector3f(0, 0, -1), 0.0f, MediumInterface());
    int occludedCount = 0, unoccludedCount = 0;
    for (int i = 0; i < 256; ++i) {
        Vector3f w;
        Float p;
        VisibilityTester v;
        one->Sample_Li(recvFront, FixedSample(i, 0), &w, &p, &v);
        if (p <= 0) continue;
        if (v.Unoccluded(scene))
            ++unoccludedCount;
        else
            ++occludedCount;
    }
    TASK_CHECK(occludedCount > 0);
    TASK_CHECK(unoccludedCount == 0);
}

// Full path: GeometricPrimitive + Scene + EstimateDirect, checking the
// reflected radiance on a simple diffuse receiver against an independent
// direct integral.  A non-uniform map is used so a constant-emitter
// approximation cannot pass.
TEST(EmissiveMapTransport, SceneEstimateDirect) {
    const int nu = 3, nv = 3;
    std::vector<Spectrum> map = MakeMap(nu, nv, [](int i, int j) {
        return Spectrum(0.6f + 1.2f * i + 0.9f * j + 1.5f * i * j);
    });
    // Light triangle in the z=0 plane.
    Point3f P[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    Point2f uv[3] = {{0, 0}, {1, 0}, {0, 1}};
    auto lightShape = MakeSimpleTriangle(P, uv);
    ASSERT_NE(nullptr, lightShape);
    const Float Lscale = 1.0f;
    auto light = MakeLight(Spectrum(Lscale), lightShape, nu, nv, map);
    TASK_CHECK(light->IsValid());

    // Receiver: a large triangle at z=5, facing down (normal -z), so it is
    // lit from above.  The triangle (Q[0],Q[1],Q[2]) lies in the +z-facing
    // winding, so reverse orientation gives the desired -z normal.
    Point3f Q[3] = {{-6, -6, 5}, {8, -6, 5}, {-6, 8, 5}};
    int qidx[3] = {0, 1, 2};
    auto recvShape = MakeTriangle(Transform(), Q, qidx, 3, nullptr,
                                  /*reverse=*/true);
    ASSERT_NE(nullptr, recvShape);
    TASK_CHECK(recvShape->Area() > 0);
    std::shared_ptr<Texture<Spectrum>> Kd =
        std::make_shared<ConstantTexture<Spectrum>>(Spectrum(0.5f));
    std::shared_ptr<Texture<Float>> sigma =
        std::make_shared<ConstantTexture<Float>>(0.f);
    std::shared_ptr<Material> mat =
        std::make_shared<MatteMaterial>(Kd, sigma, nullptr);

    std::vector<std::shared_ptr<Primitive>> prims;
    prims.push_back(std::make_shared<GeometricPrimitive>(
        recvShape, mat, nullptr, MediumInterface()));
    prims.push_back(std::make_shared<GeometricPrimitive>(
        lightShape, nullptr, light, MediumInterface()));
    auto bvh = std::make_shared<BVHAccel>(prims);
    std::vector<std::shared_ptr<Light>> lights = {light};
    Scene scene(bvh, lights);

    // Find the receiver point via the real scene intersection.  The receiver
    // faces -z, so view it from below (z < 5) looking up; this makes the
    // shading-frame outgoing direction point along -z, matching the normal.
    Point3f pRecv(0.33f, 0.31f, 5.0f);
    Ray ray(Point3f(0.33f, 0.31f, 4.0f), Vector3f(0, 0, 1));
    SurfaceInteraction isect;
    TASK_CHECK(scene.Intersect(ray, &isect));
    MemoryArena arena;
    isect.ComputeScatteringFunctions(ray, arena);
    TASK_CHECK(isect.bsdf != nullptr);
    // Confirm the receiver's GeometricPrimitive is the diffuse one.
    TASK_CHECK(isect.primitive != nullptr);

    ZeroTwoSequenceSampler sampler(16, 4);
    sampler.StartPixel(Point2i(0, 0));

    const int N = 262144;
    Spectrum L(0.f);
    for (int i = 0; i < N; ++i) {
        Point2f uLight(FixedSample(i, 0));
        Point2f uScatter(FixedSample(i, 2));
        L += EstimateDirect(isect, uScatter, *light, uLight, scene, sampler,
                            arena);
    }
    L = L / (Float)N;

    // Independent reference: Lo = (albedo/pi) * int L_e cos_l cos_r / r^2 dA_l.
    // The receiver is diffuse with Kd=0.5, and both surfaces are planar.
    const int K = 4000;
    double integral = 0;
    // Integrate over the light's UV triangle with the exact affine mapping
    // supplied by the light (UVToWorld).
    for (int a = 0; a < K; ++a) {
        for (int b = 0; b < K; ++b) {
            double s = (a + 0.5) / K, t = (b + 0.5) / K;
            double su = std::sqrt(s);
            double b1 = 1.0 - su, b2 = t * su, b0 = 1.0 - b1 - b2;
            Point2f uvq((Float)(b0 * uv[0].x + b1 * uv[1].x + b2 * uv[2].x),
                        (Float)(b0 * uv[0].y + b1 * uv[1].y + b2 * uv[2].y));
            Point3f wp;
            Vector3f pe;
            light->UVToWorld(uvq, &wp, &pe);
            Vector3f wi = wp - pRecv;
            double r2 = wi.LengthSquared();
            if (r2 <= 0) continue;
            double cosL = std::abs((double)wi.z / std::sqrt(r2));
            double cosR = cosL;
            Spectrum Le = light->RadianceAtUV(uvq);
            // The (sqrt-based) sampling above maps the unit square uniformly
            // onto the texture triangle, so the mean over the K^2 samples
            // times the triangle's texture-space area is the triangle
            // integral.  With worldArea == uvArea here the Jacobian is one.
            double dA = UVArea(uv) / (double)(K * K);
            integral += Le[0] * cosL * cosR / r2 * dA;
        }
    }
    double ref = 0.5 / Pi * integral;
    double err = std::abs((double)L[0] - ref) / ref;
    printf("EmissiveMapTransport.SceneEstimateDirect: N=%d radiance=%.8f "
           "reference=%.8f relerr=%.3g\n",
           N, L[0], ref, err);
    // The reference quadrature and the fixed-sequence estimate both converge
    // to the same value; the observed error is printed above.
    TASK_CHECK(err < 2e-3);

    // Sanity: the result is not the constant-emitter approximation, i.e. the
    // map's average differs measurably from its value at the receiver's
    // direction.  Compare against a constant light using the map's mean.
    double mean = 0;
    for (int c = 0; c < (int)map.size(); ++c) mean += map[c][0];
    mean /= map.size();
    TASK_CHECK(std::abs(mean - map[0][0]) > 0.5);
}
