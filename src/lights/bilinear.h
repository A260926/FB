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

#if defined(_MSC_VER)
#define NOMINMAX
#pragma once
#endif

#ifndef PBRT_LIGHTS_BILINEAR_H
#define PBRT_LIGHTS_BILINEAR_H

// lights/bilinear.h*
#include "pbrt.h"
#include "light.h"
#include "primitive.h"
#include "shape.h"

namespace pbrt {

class Triangle;

// BilinearAreaLight Declarations
//
// An area light for a single, static, non-degenerate triangle without alpha
// masking whose emission follows a low-resolution RGB map defined on the
// triangle's texture-coordinate grid.  The map is interpolated bilinearly
// between the grid points UV=(i/(nu-1),j/(nv-1)); the emitted radiance at
// texture coordinate (uv) is E(u,v) = L * scale * map(u,v).
//
// Positions are sampled on the triangle with world-space area density
// pA(x) = Y(E(x)) / Z, with Z = integral over the triangle of Y(E) dA.  The
// sampling distribution is built once, at construction, from exact
// polynomial integrals of the piecewise-bilinear field, and is shared
// read-only by all rendering threads.
class BilinearAreaLight : public AreaLight {
  public:
    // BilinearAreaLight Public Methods
    //
    // `L` is the complete emitted-radiance scale factor (the caller folds the
    // "scale" parameter into it); `map` has nu*nv entries, indexed as
    // map[j*nu+i] at texture coordinate (i/(nu-1), j/(nv-1)).
    BilinearAreaLight(const Transform &LightToWorld,
                      const MediumInterface &mediumInterface, const Spectrum &L,
                      int nSamples, const std::shared_ptr<Shape> &shape, int nu,
                      int nv, const std::vector<Spectrum> &map,
                      bool twoSided = false);
    BilinearAreaLight(const Transform &LightToWorld,
                      const MediumInterface &mediumInterface, const Spectrum &L,
                      int nSamples, const std::shared_ptr<Shape> &shape, int nu,
                      int nv, const Spectrum *map, bool twoSided = false)
        : BilinearAreaLight(LightToWorld, mediumInterface, L, nSamples, shape,
                            nu, nv,
                            std::vector<Spectrum>(map, map + (size_t)nu * nv),
                            twoSided) {}

    Spectrum L(const Interaction &intr, const Vector3f &w) const;
    Spectrum Power() const;
    Spectrum Sample_Li(const Interaction &ref, const Point2f &u, Vector3f *wo,
                       Float *pdf, VisibilityTester *vis) const;
    Float Pdf_Li(const Interaction &ref, const Vector3f &wi) const;
    Spectrum Sample_Le(const Point2f &u1, const Point2f &u2, Float time,
                       Ray *ray, Normal3f *nLight, Float *pdfPos,
                       Float *pdfDir) const;
    void Pdf_Le(const Ray &ray, const Normal3f &nLight, Float *pdfPos,
                Float *pdfDir) const;

    // Read-only access to configuration and derived quantities; mostly of
    // use to tests and diagnostics.
    int Nu() const { return nu; }
    int Nv() const { return nv; }
    bool IsTwoSided() const { return twoSided; }
    bool IsValid() const { return valid; }
    const std::string &ErrorString() const { return error; }
    Normal3f GeometricNormalValue() const { return nGeom; }
    Float WorldArea() const { return (Float)worldArea; }
    // The supplied map at grid point (i,j), without the L and scale factors.
    Spectrum MapValue(int i, int j) const { return rawMap[(size_t)j * nu + i]; }
    // E(u,v), including L and scale.
    Spectrum RadianceAtUV(const Point2f &uv) const {
        return RadianceAtUVInternal(uv);
    }
    // Y(E(u,v)); the bilinear field is defined outside the triangle, too.
    Float YAtUV(const Point2f &uv) const;
    // pA at the given texture coordinate (world-area measure).  Zero in
    // black regions (and zero everywhere if the triangle is entirely black).
    // Only meaningful inside the triangle's support.
    Float AreaPdfUV(const Point2f &uv) const;
    // pA(u,v) expressed as a density in the (u,v) parametrisation, i.e.
    // pA(x(u,v)) * |dA_world/dA_uv|.  Integrates to one over the UV triangle.
    Float AreaPdfUVNormalized(const Point2f &uv) const;
    // Recover the texture coordinate of a world-space point on (or
    // numerically near) the triangle.  Returns false if the point lies
    // outside the triangle's support beyond numerical tolerance.
    bool WorldToUV(const Point3f &p, Point2f *uv) const;
    // World-space position and interpolation error for a texture coordinate
    // inside the triangle.
    void UVToWorld(const Point2f &uv, Point3f *p, Vector3f *pError) const;

  private:
    // A bilinear polynomial f(u,v)=a+b*u+c*v+d*u*v, in global (u,v).
    struct CellPoly {
        double a = 0, b = 0, c = 0, d = 0;
    };
    // A piece of constant geometry in a map row.  The conditional density of
    // u at a fixed v satisfies u in [lo(v), hi(v)], where both bounds are
    // linear in v and the bilinear Y(E) coefficients are constant.
    struct VSlab {
        double vA = 0, vB = 0;
        // Marginal density of v, in t=v-vA: M[0]+M[1]t+M[2]t^2+M[3]t^3.
        double M[4] = {0, 0, 0, 0};
        double mass = 0;
    };

    void Init(const std::vector<Spectrum> &map, const Spectrum &L);
    void BuildSamplingDistribution();
    // Cumulative integral of the slab's marginal, in t = v - vA.
    static double SlabCDF(const VSlab &slab, double t);
    bool SamplePosition(const Point2f &u, Point2f *uv, Point3f *p,
                        Normal3f *n, Vector3f *pError, Float *pdfArea) const;
    Spectrum RadianceAtUVInternal(const Point2f &uv) const;

    // BilinearAreaLight Private Data
    const bool twoSided;
    std::shared_ptr<Shape> shape;
    const Triangle *triangle = nullptr;
    bool valid = false;
    std::string error;
    int nu = 0, nv = 0;
    std::vector<Spectrum> rawMap;  // supplied map (unscaled)
    std::vector<Spectrum> values;  // L*scale*map at the grid points
    std::vector<double> ygrid;     // Y(values) at the grid points
    // Bilinear Y(E) polynomial for each map cell, row-major [j*(nu-1)+i].
    std::vector<CellPoly> cellY;

    // World-space triangle vertices and original texture coordinates.
    Point3f p[3];
    Point2f uv[3];
    Normal3f nGeom;
    // Affine barycentric texture-coordinate -> world mapping.
    Vector2f uvE1, uvE2;  // uv1-uv0, uv2-uv0
    double uvDet = 0;    // cross2(uvE1,uvE2)
    double worldArea = 0, uvArea = 0, jacobian = 1;
    double Zuv = 0;      // integral of Y(E) over the UV triangle
    double Z = 0;        // integral of Y(E) over the world triangle
    Spectrum integralUV; // integral of E over the UV triangle

    std::vector<VSlab> slabs;
    std::vector<double> slabCDF;  // cumulative mass through each slab
    // True when the triangle carries no emission at all (Z==0), or when the
    // numerical mass is unusable; sampling then falls back to a uniform area
    // distribution over the triangle.
    bool uniformFallback = true;
    double vMinCache = 0, vMaxCache = 0;
};

std::shared_ptr<AreaLight> CreateBilinearAreaLight(
    const Transform &light2world, const Medium *medium,
    const ParamSet &paramSet, const std::shared_ptr<Shape> &shape);

}  // namespace pbrt

#endif  // PBRT_LIGHTS_BILINEAR_H
