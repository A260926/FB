/*
    pbrt source code is Copyright(c) 1998-2016
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

// lights/bilinear.cpp*
#include "lights/bilinear.h"
#include "interaction.h"
#include "paramset.h"
#include "sampling.h"
#include "shapes/triangle.h"
#include "stats.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace pbrt {

namespace {

inline double Cross2(const Vector2f &a, const Vector2f &b) {
    return (double)a.x * (double)b.y - (double)a.y * (double)b.x;
}

// Intersect the line v=vv with the triangle given by its texture coordinates.
// Returns false if there is no intersection; otherwise returns the extremal
// u values (which coincide at a vertex).
bool TriangleUSpan(const Point2f uvs[3], double vv, double *uLo, double *uHi) {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    bool any = false;
    for (int e = 0; e < 3; ++e) {
        const Point2f &p0 = uvs[e], &p1 = uvs[(e + 1) % 3];
        double y0 = p0.y, y1 = p1.y;
        if (y0 == y1) {
            if (vv == y0) {
                lo = std::min(lo, std::min((double)p0.x, (double)p1.x));
                hi = std::max(hi, std::max((double)p0.x, (double)p1.x));
                any = true;
            }
        } else if (vv >= std::min(y0, y1) && vv <= std::max(y0, y1)) {
            double t = (vv - y0) / (y1 - y0);
            double u = p0.x + t * (p1.x - p0.x);
            lo = std::min(lo, u);
            hi = std::max(hi, u);
            any = true;
        }
    }
    if (!any) return false;
    *uLo = lo;
    *uHi = hi;
    return true;
}

// Bilinear coefficients of a field in a grid cell, in global texture
// coordinates: f(u,v) = a + b u + c v + d u v.  The interpolation is
// f = f00 + du*(f10-f00) + dv*(f01-f00) + du*dv*(f11-f10-f01+f00) with
// du=(u-u0)/(u1-u0) and dv=(v-v0)/(v1-v0); expanding in global monomials
// produces extra cross terms that must be folded into a, b, and c.
void CellPolyFromCorners(double f00, double f10, double f01, double f11,
                         double u0, double u1, double v0, double v1, double *a,
                         double *b, double *c, double *d) {
    double du = u1 - u0, dv = v1 - v0;
    double bu = (f10 - f00) / du;
    double cv = (f01 - f00) / dv;
    double m = (f00 - f10 - f01 + f11) / (du * dv);  // mixed coefficient
    *d = m;
    *b = bu - m * v0;
    *c = cv - m * u0;
    *a = f00 - bu * u0 - cv * v0 + m * u0 * v0;
}

// Given a polynomial f(u,v) = a + b u + c v + d u v and the linear bounds
// lo(v)=l0+l1 v, hi(v)=h0+h1 v, compute q(v) = int_{lo(v)}^{hi(v)} f du.
// q is quadratic in v, so q[3] is always zero; the array is sized for a cubic
// because callers add such contributions together.
void ColumnPoly(double a, double b, double c, double d, double l0, double l1,
                double h0, double h1, double q[4]) {
    double H0 = h0 - l0, H1 = h1 - l1;
    double S0 = 0.5 * (h0 * h0 - l0 * l0);
    double S1 = h0 * h1 - l0 * l1;
    double S2 = 0.5 * (h1 * h1 - l1 * l1);
    q[0] = a * H0 + b * S0;
    q[1] = a * H1 + c * H0 + b * S1 + d * S0;
    q[2] = c * H1 + b * S2 + d * S1;
    q[3] = d * S2;
}

double IntegratePolyVV(const double q[4], double vA, double vB) {
    double vA2 = vA * vA, vB2 = vB * vB;
    double vA3 = vA2 * vA, vB3 = vB2 * vB;
    double vA4 = vA3 * vA, vB4 = vB3 * vB;
    return q[0] * (vB - vA) + q[1] * (vB2 - vA2) * 0.5 +
           q[2] * (vB3 - vA3) * (1.0 / 3.0) + q[3] * (vB4 - vA4) * 0.25;
}

// Re-express q(v) in the local variable t=v-vA.
void ShiftPoly(const double q[4], double vA, double out[4]) {
    double v2 = vA * vA, v3 = v2 * vA;
    out[0] = q[0] + q[1] * vA + q[2] * v2 + q[3] * v3;
    out[1] = q[1] + 2 * q[2] * vA + 3 * q[3] * v2;
    out[2] = q[2] + 3 * q[3] * vA;
    out[3] = q[3];
}

}  // namespace

BilinearAreaLight::BilinearAreaLight(const Transform &LightToWorld,
                                     const MediumInterface &mediumInterface,
                                     const Spectrum &L, int nSamples,
                                     const std::shared_ptr<Shape> &shape, int nu,
                                     int nv, const std::vector<Spectrum> &map,
                                     bool twoSided)
    : AreaLight(LightToWorld, mediumInterface, nSamples),
      twoSided(twoSided),
      shape(shape),
      nu(nu),
      nv(nv),
      rawMap(map) {
    Init(map, L);
}

void BilinearAreaLight::Init(const std::vector<Spectrum> &map,
                             const Spectrum &L) {
    // Only a single, static, non-degenerate triangle without alpha masking,
    // together with a well-formed emissive map, is supported.  Any failure
    // leaves the light invalid; the factory reports the reason and returns
    // null rather than silently reverting to a constant emitter.
    if (nu < 2 || nv < 2) {
        error = "nu and nv must be at least 2";
        return;
    }
    const long long expected = (long long)nu * (long long)nv;
    if (expected > (long long)std::numeric_limits<int>::max()) {
        error = "map dimensions overflow an integer";
        return;
    }
    if ((long long)map.size() != expected) {
        error = StringPrintf(
            "map has %d entries but nu*nv = %d entries are required",
            (int)map.size(), (int)expected);
        return;
    }

    triangle = dynamic_cast<const Triangle *>(shape.get());
    if (triangle == nullptr) {
        error = "a triangle shape is required";
        return;
    }
    if (triangle->HasAlphaMask()) {
        error = "alpha-masked triangles are not supported";
        return;
    }

    // World-space vertices, and a non-degenerate triangle.
    for (int i = 0; i < 3; ++i) {
        p[i] = triangle->P(i);
        if (p[i].HasNaNs()) {
            error = "triangle has NaN vertices";
            return;
        }
    }
    if (!(Cross(p[1] - p[0], p[2] - p[0]).LengthSquared() > 0)) {
        error = "a non-degenerate triangle is required";
        return;
    }

    // Texture coordinates: non-degenerate and inside [0,1]^2.
    for (int i = 0; i < 3; ++i) {
        uv[i] = triangle->UV(i);
        if (uv[i].HasNaNs()) {
            error = "triangle has NaN texture coordinates";
            return;
        }
        if (!(uv[i].x >= 0 && uv[i].x <= 1 && uv[i].y >= 0 && uv[i].y <= 1)) {
            error = StringPrintf(
                "all triangle UVs must lie in [0,1]^2 (got (%.6f, %.6f))",
                uv[i].x, uv[i].y);
            return;
        }
    }
    uvE1 = uv[1] - uv[0];
    uvE2 = uv[2] - uv[0];
    uvDet = Cross2(uvE1, uvE2);
    if (!(std::abs(uvDet) > 0)) {
        error = "non-degenerate UV coordinates are required";
        return;
    }
    uvArea = 0.5 * std::abs(uvDet);

    // Colour values must be finite and non-negative.  L is the complete
    // emissive scale (L*scale from the scene parameters).
    for (int c = 0; c < Spectrum::nSamples; ++c) {
        if (!std::isfinite(L[c]) || L[c] < 0) {
            error = "L must be finite and non-negative";
            return;
        }
    }
    for (const Spectrum &s : map) {
        for (int c = 0; c < Spectrum::nSamples; ++c) {
            Float v = s[c];
            if (!std::isfinite(v) || v < 0) {
                error = "map colours must be finite and non-negative";
                return;
            }
        }
    }

    // Emissive values at the grid points: L*scale*map.
    values.resize(map.size());
    ygrid.resize(map.size());
    for (size_t i = 0; i < map.size(); ++i) {
        Spectrum v = map[i] * L;
        values[i] = v;
        ygrid[i] = (double)v.y();
    }

    const double duGrid = 1.0 / (nu - 1);
    const double dvGrid = 1.0 / (nv - 1);
    cellY.resize((size_t)(nu - 1) * (nv - 1));
    for (int j = 0; j < nv - 1; ++j) {
        for (int i = 0; i < nu - 1; ++i) {
            int idx00 = j * nu + i;
            CellPoly &cp = cellY[(size_t)j * (nu - 1) + i];
            CellPolyFromCorners(ygrid[idx00], ygrid[idx00 + 1],
                                ygrid[idx00 + nu], ygrid[idx00 + nu + 1],
                                i * duGrid, (i + 1) * duGrid, j * dvGrid,
                                (j + 1) * dvGrid, &cp.a, &cp.b, &cp.c, &cp.d);
        }
    }

    nGeom = triangle->GeometricNormal();
    if (!(nGeom.LengthSquared() > 0)) {
        error = "could not compute a geometric normal";
        return;
    }
    worldArea = (double)triangle->Area();
    if (!(worldArea > 0) || !std::isfinite(worldArea)) {
        error = "the triangle must have finite positive area";
        return;
    }
    jacobian = worldArea / uvArea;
    if (!(jacobian > 0) || !std::isfinite(jacobian)) {
        error = "invalid UV-to-world scale";
        return;
    }

    valid = true;
    BuildSamplingDistribution();
}

//
// Distribution construction.  We sample texture coordinates (u,v) with
// density Y(E(u,v))/Zuv and map them affinely to world positions, giving
// exactly the required world-area density pA(x)=Y(E(x))/Z with
// Z = Zuv * jacobian.
//
// The UV triangle is partitioned into horizontal slabs by the grid rows and
// triangle vertices.  Within a slab the left/right boundaries are linear in
// v.  Subdividing further where those boundaries cross interior grid columns
// makes the set of active cells constant on each piece, so on each piece the
// field is a bilinear polynomial and the u integral is an exact polynomial in
// v of degree at most three.  The v marginal built this way is exact, and
// inverting it with a bounded bisection introduces no rejection sampling and
// no fixed-grid quadrature.
//
void BilinearAreaLight::BuildSamplingDistribution() {
    slabs.clear();
    slabCDF.clear();
    Zuv = Z = 0;
    integralUV = Spectrum(0.f);
    uniformFallback = true;

    const double duGrid = 1.0 / (nu - 1);
    const double dvGrid = 1.0 / (nv - 1);

    const double vMin = std::min(uv[0].y, std::min(uv[1].y, uv[2].y));
    const double vMax = std::max(uv[0].y, std::max(uv[1].y, uv[2].y));
    vMinCache = vMin;
    vMaxCache = vMax;

    std::vector<double> breaks;
    breaks.push_back(vMin);
    breaks.push_back(vMax);
    for (int j = 1; j <= nv - 2; ++j) {
        double vv = j * dvGrid;
        if (vv > vMin && vv < vMax) breaks.push_back(vv);
    }
    for (int k = 0; k < 3; ++k) {
        double vv = uv[k].y;
        if (vv > vMin && vv < vMax) breaks.push_back(vv);
    }
    std::sort(breaks.begin(), breaks.end());

    std::vector<double> edges;
    for (double vv : breaks) {
        if (edges.empty() || (vv - edges.back()) > 1e-12) edges.push_back(vv);
    }
    if (edges.size() < 2) {
        valid = false;
        error = "could not partition the UV triangle";
        return;
    }
    edges.front() = vMin;
    edges.back() = vMax;

    std::vector<double> eIntegral(Spectrum::nSamples, 0.0);

    for (size_t s = 0; s + 1 < edges.size(); ++s) {
        const double vA = edges[s], vB = edges[s + 1];
        if (!(vB > vA)) continue;

        double uLA, uHA, uLB, uHB;
        if (!TriangleUSpan(uv, vA, &uLA, &uHA)) continue;
        if (!TriangleUSpan(uv, vB, &uLB, &uHB)) continue;
        const double mL = (uLB - uLA) / (vB - vA);
        const double bL = uLA - mL * vA;
        const double mR = (uHB - uHA) / (vB - vA);
        const double bR = uHA - mR * vA;

        const double vMid = 0.5 * (vA + vB);
        int jRow = (int)std::floor(vMid / dvGrid);
        jRow = std::min(std::max(jRow, 0), nv - 2);
        const double v0 = jRow * dvGrid, v1 = (jRow + 1) * dvGrid;

        // Splits where either boundary crosses an interior grid column.
        std::vector<double> cuts;
        cuts.push_back(vA);
        cuts.push_back(vB);
        for (int i = 1; i <= nu - 2; ++i) {
            const double gu = i * duGrid;
            if (mL != 0) {
                double vv = (gu - bL) / mL;
                if (vv > vA && vv < vB) cuts.push_back(vv);
            }
            if (mR != 0) {
                double vv = (gu - bR) / mR;
                if (vv > vA && vv < vB) cuts.push_back(vv);
            }
        }
        std::sort(cuts.begin(), cuts.end());

        for (size_t c = 0; c + 1 < cuts.size(); ++c) {
            const double sA = cuts[c], sB = cuts[c + 1];
            if (!(sB > sA)) continue;
            const double sMid = 0.5 * (sA + sB);
            const double uLm = mL * sMid + bL;
            const double uRm = mR * sMid + bR;
            if (!(uRm > uLm)) continue;
            int iLo = (int)std::floor(uLm / duGrid);
            int iHi = (int)std::floor(uRm / duGrid);
            iLo = std::min(std::max(iLo, 0), nu - 2);
            iHi = std::min(std::max(iHi, 0), nu - 2);

            // The set of active columns is constant on this sub-interval, so
            // the marginal there is the sum of the corresponding column
            // polynomials.  Different sub-intervals in general have different
            // active columns, so each becomes its own sampling piece with its
            // own polynomial -- accumulating across cuts would extend columns
            // beyond where they are actually present.
            double Q[4] = {0, 0, 0, 0};
            for (int i = iLo; i <= iHi; ++i) {
                const double ui0 = i * duGrid, ui1 = (i + 1) * duGrid;
                double l0, l1, h0, h1;
                if (i == iLo) {
                    l0 = bL;
                    l1 = mL;
                } else {
                    l0 = ui0;
                    l1 = 0;
                }
                if (i == iHi) {
                    h0 = bR;
                    h1 = mR;
                } else {
                    h0 = ui1;
                    h1 = 0;
                }

                const CellPoly &cp = cellY[(size_t)jRow * (nu - 1) + i];
                double q[4];
                ColumnPoly(cp.a, cp.b, cp.c, cp.d, l0, l1, h0, h1, q);
                for (int k = 0; k < 4; ++k) Q[k] += q[k];

                // Integral of E over the same region, per channel.
                const int idx00 = jRow * nu + i;
                for (int ch = 0; ch < Spectrum::nSamples; ++ch) {
                    double ea, eb, ec, ed;
                    CellPolyFromCorners(values[idx00][ch],
                                        values[idx00 + 1][ch],
                                        values[idx00 + nu][ch],
                                        values[idx00 + nu + 1][ch], ui0, ui1,
                                        v0, v1, &ea, &eb, &ec, &ed);
                    double eq[4];
                    ColumnPoly(ea, eb, ec, ed, l0, l1, h0, h1, eq);
                    eIntegral[ch] += IntegratePolyVV(eq, sA, sB);
                }
            }

            VSlab slab;
            slab.vA = sA;
            slab.vB = sB;
            ShiftPoly(Q, sA, slab.M);
            const double T = sB - sA;
            double mass = SlabCDF(slab, T);
            if (!std::isfinite(mass)) {
                valid = false;
                error = "produced a non-finite sampling mass";
                return;
            }
            if (mass <= 0) continue;
            slab.mass = mass;
            Zuv += mass;
            slabs.push_back(slab);
        }
    }

    if (!(std::isfinite(Zuv) && Zuv > 0)) {
        // Entirely black (or numerically unusable): uniform area fallback.
        uniformFallback = true;
        Zuv = Z = 0;
        for (int ch = 0; ch < Spectrum::nSamples; ++ch)
            integralUV[ch] = (Float)eIntegral[ch];
        return;
    }

    Z = Zuv * jacobian;
    if (!std::isfinite(Z)) {
        uniformFallback = true;
        Zuv = Z = 0;
        for (int ch = 0; ch < Spectrum::nSamples; ++ch)
            integralUV[ch] = (Float)eIntegral[ch];
        return;
    }

    slabCDF.resize(slabs.size() + 1);
    slabCDF[0] = 0;
    for (size_t i = 0; i < slabs.size(); ++i)
        slabCDF[i + 1] = slabCDF[i] + slabs[i].mass;
    double total = slabCDF.back();
    if (!(total > 0) || !std::isfinite(total)) {
        uniformFallback = true;
        Zuv = Z = 0;
        for (int ch = 0; ch < Spectrum::nSamples; ++ch)
            integralUV[ch] = (Float)eIntegral[ch];
        return;
    }
    double scale = Zuv / total;
    for (size_t i = 0; i < slabCDF.size(); ++i) slabCDF[i] *= scale;

    for (int ch = 0; ch < Spectrum::nSamples; ++ch) {
        Float v = (Float)eIntegral[ch];
        if (!std::isfinite(v)) {
            valid = false;
            error = "produced a non-finite colour integral";
            return;
        }
        integralUV[ch] = v;
    }
    uniformFallback = false;
}

double BilinearAreaLight::SlabCDF(const VSlab &slab, double t) {
    double t2 = t * t, t3 = t2 * t, t4 = t3 * t;
    return slab.M[0] * t + slab.M[1] * t2 * 0.5 +
           slab.M[2] * t3 * (1.0 / 3.0) + slab.M[3] * t4 * 0.25;
}

Float BilinearAreaLight::YAtUV(const Point2f &uvc) const {
    double x = (double)uvc.x * (nu - 1);
    double y = (double)uvc.y * (nv - 1);
    int i = (int)std::floor(x), j = (int)std::floor(y);
    i = std::min(std::max(i, 0), nu - 2);
    j = std::min(std::max(j, 0), nv - 2);
    double tx = x - i, ty = y - j;
    double v00 = ygrid[j * nu + i];
    double v10 = ygrid[j * nu + i + 1];
    double v01 = ygrid[(j + 1) * nu + i];
    double v11 = ygrid[(j + 1) * nu + i + 1];
    return (Float)((1 - tx) * (1 - ty) * v00 + tx * (1 - ty) * v10 +
                   (1 - tx) * ty * v01 + tx * ty * v11);
}

Spectrum BilinearAreaLight::RadianceAtUVInternal(const Point2f &uvc) const {
    double x = (double)uvc.x * (nu - 1);
    double y = (double)uvc.y * (nv - 1);
    int i = (int)std::floor(x), j = (int)std::floor(y);
    i = std::min(std::max(i, 0), nu - 2);
    j = std::min(std::max(j, 0), nv - 2);
    double tx = x - i, ty = y - j;
    Spectrum s00 = values[j * nu + i];
    Spectrum s10 = values[j * nu + i + 1];
    Spectrum s01 = values[(j + 1) * nu + i];
    Spectrum s11 = values[(j + 1) * nu + i + 1];
    return (1 - tx) * (1 - ty) * s00 + tx * (1 - ty) * s10 +
           (1 - tx) * ty * s01 + tx * ty * s11;
}

Float BilinearAreaLight::AreaPdfUV(const Point2f &uvc) const {
    // pA in world-area measure.  The caller must only query this inside the
    // triangle's texture-coordinate support.
    if (uniformFallback) return (Float)(1.0 / worldArea);
    return (Float)((double)YAtUV(uvc) / Z);
}

Float BilinearAreaLight::AreaPdfUVNormalized(const Point2f &uvc) const {
    // pA as a density in the (u,v) parametrisation: pA(x(u,v)) * |dA/dudv|.
    // This integrates to 1 over the UV triangle, so it can be compared
    // directly against sampling frequencies binned in UV space.
    return AreaPdfUV(uvc) * (Float)jacobian;
}

void BilinearAreaLight::UVToWorld(const Point2f &uvc, Point3f *pos,
                                  Vector3f *pError) const {
    double b1 = Cross2(uvc - uv[0], uvE2) / uvDet;
    double b2 = Cross2(uvE1, uvc - uv[0]) / uvDet;
    double b0 = 1.0 - b1 - b2;
    *pos = (Float)b0 * p[0] + (Float)b1 * p[1] + (Float)b2 * p[2];
    Point3f pAbsSum = Abs((Float)b0 * p[0]) + Abs((Float)b1 * p[1]) +
                      Abs((Float)b2 * p[2]);
    *pError = gamma(6) * Vector3f(pAbsSum.x, pAbsSum.y, pAbsSum.z);
}

bool BilinearAreaLight::WorldToUV(const Point3f &pos, Point2f *uvOut) const {
    // Recover the texture coordinate from the world position.  Project the
    // point onto the triangle's plane first: Sample_Le offsets its ray origin
    // along the normal, so the query point is only near the plane.
    double planeDist = Dot(nGeom, pos - p[0]);
    double planeTol =
        1e-4 * std::max(1.0, (double)Vector3f(pos).Length());
    if (!(std::abs(planeDist) <= planeTol)) return false;
    Point3f q = pos - (Float)planeDist * Vector3f(nGeom);

    Vector3f e1 = p[1] - p[0], e2 = p[2] - p[0];
    double g11 = Dot(e1, e1), g12 = Dot(e1, e2), g22 = Dot(e2, e2);
    double det = g11 * g22 - g12 * g12;
    if (!(det > 0)) return false;
    Vector3f r = q - p[0];
    double r1 = Dot(e1, r), r2 = Dot(e2, r);
    double b1 = (r1 * g22 - r2 * g12) / det;
    double b2 = (r2 * g11 - r1 * g12) / det;
    double b0 = 1.0 - b1 - b2;
    const double tol = 1e-5;
    if (b0 < -tol || b1 < -tol || b2 < -tol) return false;

    Point2f u = uv[0] + (Float)b1 * uvE1 + (Float)b2 * uvE2;
    if (!(u.x >= -tol && u.x <= 1 + tol && u.y >= -tol && u.y <= 1 + tol))
        return false;
    *uvOut = Point2f((Float)Clamp((double)u.x, 0.0, 1.0),
                     (Float)Clamp((double)u.y, 0.0, 1.0));
    return true;
}

Spectrum BilinearAreaLight::L(const Interaction &intr,
                              const Vector3f &w) const {
    if (!valid) return Spectrum(0.f);
    Point2f uvc;
    if (!WorldToUV(intr.p, &uvc)) return Spectrum(0.f);
    if (!twoSided && !(Dot(nGeom, w) > 0)) return Spectrum(0.f);
    return RadianceAtUVInternal(uvc);
}

Spectrum BilinearAreaLight::Power() const {
    if (!valid) return Spectrum(0.f);
    return (twoSided ? 2.f : 1.f) * Pi * jacobian * integralUV;
}

bool BilinearAreaLight::SamplePosition(const Point2f &u, Point2f *uvOut,
                                       Point3f *pos, Normal3f *n,
                                       Vector3f *pError, Float *pdfArea) const {
    *n = nGeom;
    if (uniformFallback) {
        // Uniform over the triangle's area; used only for fully black lights,
        // where the returned radiance is black and the power is zero.
        Point2f b = UniformSampleTriangle(u);
        double b0 = 1.0 - (double)b.x - (double)b.y;
        double b1 = b.x;
        double b2 = b.y;
        *pos = (Float)b0 * p[0] + (Float)b1 * p[1] + (Float)b2 * p[2];
        Point3f pAbsSum = Abs((Float)b0 * p[0]) + Abs((Float)b1 * p[1]) +
                          Abs((Float)b2 * p[2]);
        *pError = gamma(6) * Vector3f(pAbsSum.x, pAbsSum.y, pAbsSum.z);
        *uvOut = uv[0] + (Float)b1 * uvE1 + (Float)b2 * uvE2;
        *pdfArea = (Float)(1.0 / worldArea);
        return true;
    }

    // Choose a slab proportional to its mass.
    double x = (double)Clamp(u.x, 0.f, OneMinusEpsilon) * Zuv;
    int s;
    {
        int lo = 0, hi = (int)slabs.size();
        while (lo + 1 < hi) {
            int mid = (lo + hi) / 2;
            if (slabCDF[mid] <= x)
                lo = mid;
            else
                hi = mid;
        }
        s = std::min(lo, (int)slabs.size() - 1);
    }
    const VSlab &slab = slabs[s];
    double target = x - slabCDF[s];
    if (target < 0) target = 0;
    if (target > slab.mass) target = slab.mass;
    const double T = slab.vB - slab.vA;

    // Invert the exact polynomial CDF with a bounded bisection.
    double lo = 0, hi = T;
    for (int it = 0; it < 90; ++it) {
        double mid = 0.5 * (lo + hi);
        if (SlabCDF(slab, mid) < target)
            lo = mid;
        else
            hi = mid;
    }
    double t = 0.5 * (lo + hi);
    double v = std::min(std::max(slab.vA + t, vMinCache), vMaxCache);

    // Conditional u sampling at this v.
    double uL, uR;
    if (!TriangleUSpan(uv, v, &uL, &uR) || !(uR > uL)) {
        uL = uR = (uv[0].x + uv[1].x + uv[2].x) / 3.0;
    }
    if (!(uR > uL)) {
        // Measure-zero corner; sample the centroid texture coordinate.
        uL = uR = (uv[0].x + uv[1].x + uv[2].x) / 3.0;
    }

    const double dvGrid = 1.0 / (nv - 1), duGrid = 1.0 / (nu - 1);
    int jRow = (int)std::floor(v / dvGrid);
    jRow = std::min(std::max(jRow, 0), nv - 2);
    const double v0 = jRow * dvGrid, v1 = (jRow + 1) * dvGrid;
    int iLo = (int)std::floor(uL / duGrid);
    int iHi = (int)std::floor(uR / duGrid);
    iLo = std::min(std::max(iLo, 0), nu - 2);
    iHi = std::min(std::max(iHi, 0), nu - 2);

    // First pass: total conditional weight.
    double W = 0;
    for (int i = iLo; i <= iHi; ++i) {
        const double ui0 = i * duGrid, ui1 = (i + 1) * duGrid;
        const double l = std::max(uL, ui0), h = std::min(uR, ui1);
        if (!(h > l)) continue;
        const CellPoly &cp = cellY[(size_t)jRow * (nu - 1) + i];
        double alpha = cp.a + cp.c * v, beta = cp.b + cp.d * v;
        double w = alpha * (h - l) + beta * (h * h - l * l) * 0.5;
        if (w > 0) W += w;
    }

    double uVal;
    if (!(W > 0)) {
        uVal = uL + (uR - uL) * (double)Clamp(u.y, 0.f, OneMinusEpsilon);
    } else {
        double y = (double)Clamp(u.y, 0.f, OneMinusEpsilon) * W;
        double cum = 0;
        uVal = 0.5 * (uL + uR);
        for (int i = iLo; i <= iHi; ++i) {
            const double ui0 = i * duGrid, ui1 = (i + 1) * duGrid;
            const double l = std::max(uL, ui0), h = std::min(uR, ui1);
            if (!(h > l)) continue;
            const CellPoly &cp = cellY[(size_t)jRow * (nu - 1) + i];
            double alpha = cp.a + cp.c * v, beta = cp.b + cp.d * v;
            double w = alpha * (h - l) + beta * (h * h - l * l) * 0.5;
            if (!(w > 0)) continue;
            if (y <= cum + w || i == iHi) {
                double targetLocal = Clamp(y - cum, 0.0, w);
                double loU = l, hiU = h;
                for (int it = 0; it < 80; ++it) {
                    double mid = 0.5 * (loU + hiU);
                    double val = alpha * (mid - l) +
                                 beta * (mid * mid - l * l) * 0.5;
                    if (val < targetLocal)
                        loU = mid;
                    else
                        hiU = mid;
                }
                uVal = 0.5 * (loU + hiU);
                break;
            }
            cum += w;
        }
    }

    uVal = std::min(std::max(uVal, uL), uR);
    *uvOut = Point2f((Float)uVal, (Float)v);
    UVToWorld(*uvOut, pos, pError);
    *pdfArea = (Float)((double)YAtUV(*uvOut) / Z);
    return true;
}

Spectrum BilinearAreaLight::Sample_Li(const Interaction &ref, const Point2f &u,
                                      Vector3f *wo, Float *pdf,
                                      VisibilityTester *vis) const {
    ProfilePhase _(Prof::LightSample);
    if (!valid) {
        *pdf = 0;
        return Spectrum(0.f);
    }
    Point2f uvc;
    Point3f pos;
    Normal3f n;
    Vector3f pError;
    Float pdfArea;
    SamplePosition(u, &uvc, &pos, &n, &pError, &pdfArea);

    Interaction pShape;
    pShape.p = pos;
    pShape.n = n;
    pShape.pError = pError;
    pShape.time = ref.time;
    pShape.mediumInterface = mediumInterface;

    Vector3f d = pos - ref.p;
    if (!(pdfArea > 0) || d.LengthSquared() == 0) {
        *pdf = 0;
        return Spectrum(0.f);
    }
    *wo = Normalize(d);
    Float cos = AbsDot(nGeom, -*wo);
    if (!(cos > 0)) {
        *pdf = 0;
        return Spectrum(0.f);
    }
    *pdf = pdfArea * d.LengthSquared() / cos;
    if (std::isinf(*pdf)) *pdf = 0;
    *vis = VisibilityTester(ref, pShape);
    if (!twoSided && !(Dot(nGeom, -*wo) > 0)) return Spectrum(0.f);
    return RadianceAtUVInternal(uvc);
}

Float BilinearAreaLight::Pdf_Li(const Interaction &ref,
                                const Vector3f &wi) const {
    ProfilePhase _(Prof::LightPdf);
    if (!valid) return 0;
    Ray ray = ref.SpawnRay(wi);
    Float tHit;
    SurfaceInteraction isectLight;
    if (!shape->Intersect(ray, &tHit, &isectLight, false)) return 0;
    Float cos = AbsDot(nGeom, -wi);
    if (!(cos > 0)) return 0;
    Float pdf = AreaPdfUV(isectLight.uv) *
                DistanceSquared(ref.p, isectLight.p) / cos;
    if (std::isinf(pdf)) pdf = 0;
    return pdf;
}

Spectrum BilinearAreaLight::Sample_Le(const Point2f &u1, const Point2f &u2,
                                      Float time, Ray *ray, Normal3f *nLight,
                                      Float *pdfPos, Float *pdfDir) const {
    ProfilePhase _(Prof::LightSample);
    if (!valid) {
        *pdfPos = *pdfDir = 0;
        return Spectrum(0.f);
    }
    Point2f uvc;
    Point3f pos;
    Vector3f pError;
    Normal3f n;
    Float pdfArea;
    SamplePosition(u1, &uvc, &pos, &n, &pError, &pdfArea);
    *nLight = n;

    // Cosine-weighted outgoing direction, matching DiffuseAreaLight.
    Vector3f w;
    if (twoSided) {
        Point2f u = u2;
        if (u[0] < .5) {
            u[0] = std::min(u[0] * 2, OneMinusEpsilon);
            w = CosineSampleHemisphere(u);
        } else {
            u[0] = std::min((u[0] - .5f) * 2, OneMinusEpsilon);
            w = CosineSampleHemisphere(u);
            w.z *= -1;
        }
        *pdfDir = 0.5f * CosineHemispherePdf(std::abs(w.z));
    } else {
        w = CosineSampleHemisphere(u2);
        *pdfDir = CosineHemispherePdf(w.z);
    }

    Vector3f v1, v2;
    CoordinateSystem(Vector3f(n), &v1, &v2);
    w = w.x * v1 + w.y * v2 + w.z * Vector3f(n);

    Point3f o = OffsetRayOrigin(pos, pError, n, w);
    const Medium *med = Dot(w, Vector3f(n)) > 0 ? mediumInterface.outside
                                                : mediumInterface.inside;
    *ray = Ray(o, w, Infinity, time, med);
    *pdfPos = pdfArea;

    if (!twoSided && !(Dot(n, w) > 0)) return Spectrum(0.f);
    return RadianceAtUVInternal(uvc);
}

void BilinearAreaLight::Pdf_Le(const Ray &ray, const Normal3f &nLight,
                               Float *pdfPos, Float *pdfDir) const {
    ProfilePhase _(Prof::LightPdf);
    if (!valid) {
        *pdfPos = *pdfDir = 0;
        return;
    }
    Point2f uvc;
    if (!WorldToUV(ray.o, &uvc)) {
        *pdfPos = *pdfDir = 0;
        return;
    }
    *pdfPos = AreaPdfUV(uvc);
    if (twoSided) {
        *pdfDir = 0.5f * CosineHemispherePdf(AbsDot(nLight, ray.d));
    } else {
        Float cos = Dot(nLight, ray.d);
        *pdfDir = cos > 0 ? CosineHemispherePdf(cos) : 0.f;
    }
}

std::shared_ptr<AreaLight> CreateBilinearAreaLight(
    const Transform &light2world, const Medium *medium,
    const ParamSet &paramSet, const std::shared_ptr<Shape> &shape) {
    // Dimensions and map are required; report an error and return null rather
    // than quietly emitting a constant radiance.
    int nuCount = 0, nvCount = 0;
    const int *nuPtr = paramSet.FindInt("nu", &nuCount);
    const int *nvPtr = paramSet.FindInt("nv", &nvCount);
    if (!nuPtr || nuCount != 1) {
        Error("BilinearAreaLight: missing or invalid \"nu\" (expected one "
              "integer)");
        return nullptr;
    }
    if (!nvPtr || nvCount != 1) {
        Error("BilinearAreaLight: missing or invalid \"nv\" (expected one "
              "integer)");
        return nullptr;
    }
    const int nu = nuPtr[0], nv = nvPtr[0];
    if (nu < 2 || nv < 2) {
        Error("BilinearAreaLight: nu and nv must be at least 2 (got %d, %d)",
              nu, nv);
        return nullptr;
    }
    if ((long long)nu * (long long)nv >
        (long long)std::numeric_limits<int>::max()) {
        Error("BilinearAreaLight: nu*nv overflows an integer");
        return nullptr;
    }

    int mapCount = 0;
    const Spectrum *mapPtr = paramSet.FindSpectrum("map", &mapCount);
    if (!mapPtr) {
        Error("BilinearAreaLight: missing \"map\" spectrum array");
        return nullptr;
    }
    const long long expected = (long long)nu * (long long)nv;
    if ((long long)mapCount != expected) {
        Error("BilinearAreaLight: \"map\" has %d entries but nu*nv = %lld "
              "entries are required",
              mapCount, expected);
        return nullptr;
    }
    std::vector<Spectrum> map(mapPtr, mapPtr + mapCount);

    Spectrum L = paramSet.FindOneSpectrum("L", Spectrum(1.0));
    Spectrum sc = paramSet.FindOneSpectrum("scale", Spectrum(1.0));
    const Spectrum Le = L * sc;
    for (int c = 0; c < Spectrum::nSamples; ++c) {
        if (!std::isfinite(Le[c]) || Le[c] < 0) {
            Error("BilinearAreaLight: L*scale must be finite and non-negative");
            return nullptr;
        }
    }

    int nSamples = paramSet.FindOneInt("samples",
                                       paramSet.FindOneInt("nsamples", 1));
    const bool twoSided = paramSet.FindOneBool("twosided", false);
    if (PbrtOptions.quickRender) nSamples = std::max(1, nSamples / 4);

    std::shared_ptr<BilinearAreaLight> light =
        std::make_shared<BilinearAreaLight>(
            light2world, MediumInterface(medium), Le, nSamples, shape, nu, nv,
            map, twoSided);
    if (!light->IsValid()) {
        Error("BilinearAreaLight: %s", light->ErrorString().c_str());
        return nullptr;
    }
    return light;
}

}  // namespace pbrt
