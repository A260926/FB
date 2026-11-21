
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

// lights/infinite.cpp*
#include "lights/infinite.h"
#include "imageio.h"
#include "paramset.h"
#include "sampling.h"
#include "stats.h"

namespace pbrt {

// InfiniteAreaLight Method Definitions
InfiniteAreaLight::InfiniteAreaLight(const Transform &LightToWorld,
                                     const Spectrum &L, int nSamples,
                                     const std::string &texmap)
    : InfiniteAreaLight(LightToWorld, L, nSamples, texmap,
                        std::vector<PortalRect>(), 64, 0) {}

InfiniteAreaLight::InfiniteAreaLight(const Transform &LightToWorld,
                                     const Spectrum &L, int nSamples,
                                     const std::string &texmap,
                                     const std::vector<PortalRect> &portals,
                                     int portalResolution, Float portalMix)
    : Light((int)LightFlags::Infinite, LightToWorld, MediumInterface(),
            nSamples) {
    // Read texel data from _texmap_ and initialize _Lmap_
    Point2i resolution;
    std::unique_ptr<RGBSpectrum[]> texels(nullptr);
    if (texmap != "") {
        texels = ReadImage(texmap, &resolution);
        if (texels)
            for (int i = 0; i < resolution.x * resolution.y; ++i)
                texels[i] *= L.ToRGBSpectrum();
    }
    if (!texels) {
        resolution.x = resolution.y = 1;
        texels = std::unique_ptr<RGBSpectrum[]>(new RGBSpectrum[1]);
        texels[0] = L.ToRGBSpectrum();
    }
    Lmap.reset(new MIPMap<RGBSpectrum>(resolution, texels.get()));

    // Initialize sampling PDFs for infinite area light

    // Compute scalar-valued image _img_ from environment map
    int width = 2 * Lmap->Width(), height = 2 * Lmap->Height();
    std::unique_ptr<Float[]> img(new Float[width * height]);
    float fwidth = 0.5f / std::min(width, height);
    ParallelFor(
        [&](int64_t v) {
            Float vp = (v + .5f) / (Float)height;
            Float sinTheta = std::sin(Pi * (v + .5f) / height);
            for (int u = 0; u < width; ++u) {
                Float up = (u + .5f) / (Float)width;
                img[u + v * width] = Lmap->Lookup(Point2f(up, vp), fwidth).y();
                img[u + v * width] *= sinTheta;
            }
        },
        height, 32);

    // Compute sampling distributions for rows and columns of image
    distribution.reset(new Distribution2D(img.get(), width, height));

    // Build the portal-assisted sampling distribution, if any portals were
    // given. The environment map and its whole-map distribution above are
    // left untouched.
    if (!portals.empty())
        portalDistribution.reset(new PortalDistribution(
            portals, portalResolution, portalMix, *Lmap));
}

Spectrum InfiniteAreaLight::Power() const {
    return Pi * worldRadius * worldRadius *
           Spectrum(Lmap->Lookup(Point2f(.5f, .5f), .5f),
                    SpectrumType::Illuminant);
}

Spectrum InfiniteAreaLight::Le(const RayDifferential &ray) const {
    Vector3f w = Normalize(WorldToLight(ray.d));
    Point2f st(SphericalPhi(w) * Inv2Pi, SphericalTheta(w) * InvPi);
    return Spectrum(Lmap->Lookup(st), SpectrumType::Illuminant);
}

Spectrum InfiniteAreaLight::SampleEnv(const Interaction &ref,
                                      const Point2f &u, Vector3f *wi,
                                      Float *pdf, VisibilityTester *vis,
                                      bool *valid) const {
    // Find $(u,v)$ sample coordinates in infinite light texture
    Float mapPdf;
    Point2f uv = distribution->SampleContinuous(u, &mapPdf);
    if (mapPdf == 0) {
        *valid = false;
        *pdf = 0;
        return Spectrum(0.f);
    }
    *valid = true;

    // Convert infinite light sample point to direction
    Float theta = uv[1] * Pi, phi = uv[0] * 2 * Pi;
    Float cosTheta = std::cos(theta), sinTheta = std::sin(theta);
    Float sinPhi = std::sin(phi), cosPhi = std::cos(phi);
    *wi =
        LightToWorld(Vector3f(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta));

    // Compute PDF for sampled infinite light direction
    *pdf = mapPdf / (2 * Pi * Pi * sinTheta);
    if (sinTheta == 0) *pdf = 0;

    // Return radiance value for infinite light direction
    *vis = VisibilityTester(ref, Interaction(ref.p + *wi * (2 * worldRadius),
                                             ref.time, mediumInterface));
    return Spectrum(Lmap->Lookup(uv), SpectrumType::Illuminant);
}

Spectrum InfiniteAreaLight::Sample_Li(const Interaction &ref, const Point2f &u,
                                      Vector3f *wi, Float *pdf,
                                      VisibilityTester *vis) const {
    ProfilePhase _(Prof::LightSample);
    if (portalDistribution) {
        // Project the portals for this receiver point; if none of them
        // carry mass, fall back to plain whole-map sampling below.
        PortalDistribution::Receiver receiver =
            portalDistribution->BuildReceiver(WorldToLight(ref.p));
        if (receiver.totalMass > 0) {
            Float mix = portalDistribution->Mix();
            if (u[0] < mix) {
                // Portal branch: sample the cropped angular distribution
                // shared with Pdf_Li().
                Vector3f wl = portalDistribution->Sample(
                    receiver, Point2f(u[0] / mix, u[1]));
                *wi = LightToWorld(wl);
                Vector3f wn = Normalize(wl);
                Point2f st(SphericalPhi(wn) * Inv2Pi,
                           SphericalTheta(wn) * InvPi);
                *pdf = (1 - mix) * EnvPdf(*wi) +
                       mix * portalDistribution->Pdf(receiver, wl);
                *vis = VisibilityTester(
                    ref, Interaction(ref.p + *wi * (2 * worldRadius),
                                     ref.time, mediumInterface));
                return Spectrum(Lmap->Lookup(st), SpectrumType::Illuminant);
            }
            // Environment branch on the remapped sample; the returned
            // density still includes the portal branch so that both MIS
            // paths in EstimateDirect see the total density.
            bool valid;
            Float envPdf;
            Spectrum Li = SampleEnv(ref, Point2f((u[0] - mix) / (1 - mix), u[1]),
                                    wi, &envPdf, vis, &valid);
            if (!valid) {
                *pdf = 0;
                return Li;
            }
            *pdf = (1 - mix) * envPdf +
                   mix * portalDistribution->Pdf(receiver, WorldToLight(*wi));
            return Li;
        }
    }
    bool valid;
    return SampleEnv(ref, u, wi, pdf, vis, &valid);
}

Float InfiniteAreaLight::EnvPdf(const Vector3f &w) const {
    Vector3f wi = WorldToLight(w);
    Float theta = SphericalTheta(wi), phi = SphericalPhi(wi);
    Float sinTheta = std::sin(theta);
    if (sinTheta == 0) return 0;
    return distribution->Pdf(Point2f(phi * Inv2Pi, theta * InvPi)) /
           (2 * Pi * Pi * sinTheta);
}

Float InfiniteAreaLight::Pdf_Li(const Interaction &ref,
                                const Vector3f &w) const {
    ProfilePhase _(Prof::LightPdf);
    Float pdf = EnvPdf(w);
    if (portalDistribution) {
        PortalDistribution::Receiver receiver =
            portalDistribution->BuildReceiver(WorldToLight(ref.p));
        if (receiver.totalMass > 0) {
            Float mix = portalDistribution->Mix();
            pdf = (1 - mix) * pdf +
                  mix * portalDistribution->Pdf(receiver, WorldToLight(w));
        }
    }
    return pdf;
}

Spectrum InfiniteAreaLight::Sample_Le(const Point2f &u1, const Point2f &u2,
                                      Float time, Ray *ray, Normal3f *nLight,
                                      Float *pdfPos, Float *pdfDir) const {
    ProfilePhase _(Prof::LightSample);
    // Compute direction for infinite light sample ray
    Point2f u = u1;

    // Find $(u,v)$ sample coordinates in infinite light texture
    Float mapPdf;
    Point2f uv = distribution->SampleContinuous(u, &mapPdf);
    if (mapPdf == 0) return Spectrum(0.f);
    Float theta = uv[1] * Pi, phi = uv[0] * 2.f * Pi;
    Float cosTheta = std::cos(theta), sinTheta = std::sin(theta);
    Float sinPhi = std::sin(phi), cosPhi = std::cos(phi);
    Vector3f d =
        -LightToWorld(Vector3f(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta));
    *nLight = (Normal3f)d;

    // Compute origin for infinite light sample ray
    Vector3f v1, v2;
    CoordinateSystem(-d, &v1, &v2);
    Point2f cd = ConcentricSampleDisk(u2);
    Point3f pDisk = worldCenter + worldRadius * (cd.x * v1 + cd.y * v2);
    *ray = Ray(pDisk + worldRadius * -d, d, Infinity, time);

    // Compute _InfiniteAreaLight_ ray PDFs
    *pdfDir = sinTheta == 0 ? 0 : mapPdf / (2 * Pi * Pi * sinTheta);
    *pdfPos = 1 / (Pi * worldRadius * worldRadius);
    return Spectrum(Lmap->Lookup(uv), SpectrumType::Illuminant);
}

void InfiniteAreaLight::Pdf_Le(const Ray &ray, const Normal3f &, Float *pdfPos,
                               Float *pdfDir) const {
    ProfilePhase _(Prof::LightPdf);
    Vector3f d = -WorldToLight(ray.d);
    Float theta = SphericalTheta(d), phi = SphericalPhi(d);
    Point2f uv(phi * Inv2Pi, theta * InvPi);
    Float mapPdf = distribution->Pdf(uv);
    *pdfDir = mapPdf / (2 * Pi * Pi * std::sin(theta));
    *pdfPos = 1 / (Pi * worldRadius * worldRadius);
}

// Returns true if the 3x3 part of _t_ is orthonormal (rotation, possibly
// with reflection); translations are allowed. Portal-assisted sampling is
// only supported for such rigid LightToWorld transforms, since otherwise
// the light-local angular density would not transform trivially.
static bool IsRigidTransform(const Transform &t) {
    const Matrix4x4 &m = t.GetMatrix();
    Vector3f c0(m.m[0][0], m.m[1][0], m.m[2][0]);
    Vector3f c1(m.m[0][1], m.m[1][1], m.m[2][1]);
    Vector3f c2(m.m[0][2], m.m[1][2], m.m[2][2]);
    auto unit = [](const Vector3f &v) {
        Float l2 = v.LengthSquared();
        return l2 > 1 - 1e-3 && l2 < 1 + 1e-3;
    };
    return unit(c0) && unit(c1) && unit(c2) &&
           std::abs(Dot(c0, c1)) < 1e-3 && std::abs(Dot(c0, c2)) < 1e-3 &&
           std::abs(Dot(c1, c2)) < 1e-3;
}

std::shared_ptr<InfiniteAreaLight> CreateInfiniteLight(
    const Transform &light2world, const ParamSet &paramSet) {
    Spectrum L = paramSet.FindOneSpectrum("L", Spectrum(1.0));
    Spectrum sc = paramSet.FindOneSpectrum("scale", Spectrum(1.0));
    std::string texmap = paramSet.FindOneFilename("mapname", "");
    int nSamples = paramSet.FindOneInt("samples",
                                       paramSet.FindOneInt("nsamples", 1));
    if (PbrtOptions.quickRender) nSamples = std::max(1, nSamples / 4);

    // Optional portal-assisted sampling parameters. They are looked up even
    // when unused so that they are never reported as unused, but they are
    // only validated when portals are actually present.
    int nPortalValues = 0;
    const Float *portalValues = paramSet.FindFloat("portals", &nPortalValues);
    int portalResolution = paramSet.FindOneInt("portalresolution", 64);
    Float portalMix = paramSet.FindOneFloat("portalmix", 0.9f);

    std::vector<PortalRect> portals;
    if (portalValues) {
        // Reject invalid portal input outright rather than leaving a
        // half-initialized light behind.
        if (nPortalValues == 0 || nPortalValues % 5 != 0) {
            Error("infinite light: \"portals\" must give five values "
                  "(xmin, ymin, xmax, ymax, z) per portal, got %d.",
                  nPortalValues);
            return nullptr;
        }
        if (portalResolution < 2 || portalResolution > 512) {
            Error("infinite light: \"portalresolution\" must be in [2, 512], "
                  "got %d.",
                  portalResolution);
            return nullptr;
        }
        if (!std::isfinite(portalMix) || portalMix < 0 || portalMix >= 1) {
            Error("infinite light: \"portalmix\" must be finite and in "
                  "[0, 1), got %f.",
                  portalMix);
            return nullptr;
        }
        for (int i = 0; i < nPortalValues; i += 5) {
            PortalRect portal{portalValues[i], portalValues[i + 1],
                              portalValues[i + 2], portalValues[i + 3],
                              portalValues[i + 4]};
            if (!std::isfinite(portal.xmin) || !std::isfinite(portal.ymin) ||
                !std::isfinite(portal.xmax) || !std::isfinite(portal.ymax) ||
                !std::isfinite(portal.z)) {
                Error("infinite light: \"portals\" values must be finite.");
                return nullptr;
            }
            if (!(portal.xmax > portal.xmin) || !(portal.ymax > portal.ymin)) {
                Error("infinite light: portal rectangle must have positive "
                      "area.");
                return nullptr;
            }
            portals.push_back(portal);
        }
        if (!IsRigidTransform(light2world)) {
            Error("infinite light: \"portals\" require a rigid LightToWorld "
                  "transform.");
            return nullptr;
        }
    }
    return std::make_shared<InfiniteAreaLight>(light2world, L * sc, nSamples,
                                               texmap, portals,
                                               portalResolution, portalMix);
}

}  // namespace pbrt
