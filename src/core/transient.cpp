
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

// core/transient.cpp*
#include "transient.h"

#include <stdexcept>
#include <cstdio>
#include <fstream>
#include <iomanip>

#include "interaction.h"
#include "light.h"
#include "lights/diffuse.h"
#include "lights/point.h"
#include "media/homogeneous.h"
#include "memory.h"
#include "paramset.h"
#include "reflection.h"
#include "sampler.h"
#include "sampling.h"
#include "scene.h"
#include "stats.h"

namespace pbrt {

STAT_INT_DISTRIBUTION("Transient/Path length", transientPathLength);
STAT_COUNTER("Transient/Volume interactions", transientVolumeInteractions);
STAT_COUNTER("Transient/Surface interactions", transientSurfaceInteractions);

// TemporalPulse Method Definitions
TemporalPulse::TemporalPulse(std::vector<double> timesSeconds,
                             std::vector<double> values)
    : times(std::move(timesSeconds)), vals(std::move(values)), integral(0.0) {
    if (times.size() != vals.size())
        throw std::invalid_argument(
            "TemporalPulse: pulsetimes and pulsevalues must have the same "
            "number of entries");
    if (times.size() < 2)
        throw std::invalid_argument(
            "TemporalPulse: at least two pulse nodes are required");
    for (double t : times)
        if (!std::isfinite(t) || t < 0.0)
            throw std::invalid_argument(
                "TemporalPulse: pulse times must be finite and non-negative");
    for (size_t i = 1; i < times.size(); ++i)
        if (!(times[i] > times[i - 1]))
            throw std::invalid_argument(
                "TemporalPulse: pulse times must be strictly increasing");
    for (double v : vals)
        if (!std::isfinite(v) || v < 0.0)
            throw std::invalid_argument(
                "TemporalPulse: pulse values must be finite and non-negative");

    // Exact trapezoidal integral of the piecewise-linear shape.
    double area = 0.0;
    for (size_t i = 1; i < times.size(); ++i)
        area += 0.5 * (vals[i - 1] + vals[i]) * (times[i] - times[i - 1]);
    if (!(area > 0.0))
        throw std::invalid_argument(
            "TemporalPulse: pulse must have a positive trapezoidal integral");

    // Normalize so the pulse shape's scaling never changes the total
    // emitted energy; the light's I / L stays the full-pulse integral.
    for (double &v : vals) v /= area;
    integral = 1.0;
}

double TemporalPulse::Integral(double beginSeconds,
                               double endSeconds) const {
    if (!(endSeconds > beginSeconds)) return 0.0;
    // Exact piecewise-linear integration.  On every node interval
    // [t_i, t_{i+1}] the function is linear, so the mass of a clipped
    // sub-range [a, b] is the trapezoid 0.5*(h(a)+h(b))*(b-a).  No time
    // sampling or numerical grid is used.
    double mass = 0.0;
    for (size_t i = 1; i < times.size(); ++i) {
        const double segT0 = times[i - 1], segT1 = times[i];
        const double a = std::max(beginSeconds, segT0);
        const double b = std::min(endSeconds, segT1);
        if (b <= a) continue;
        const double h0 = vals[i - 1], h1 = vals[i];
        const double ha = h0 + (h1 - h0) * ((a - segT0) / (segT1 - segT0));
        const double hb = h0 + (h1 - h0) * ((b - segT0) / (segT1 - segT0));
        mass += 0.5 * (ha + hb) * (b - a);
    }
    return mass;
}

// Transient medium helpers
double MediumGroupIndex(const Medium *medium) {
    // Vacuum (a ray carried by no medium) has group index one.
    if (!medium) return 1.0;
    if (const HomogeneousMedium *hm =
            dynamic_cast<const HomogeneousMedium *>(medium))
        return hm->GroupIndex();
    // Heterogeneous media are rejected at scene validation; never invent a
    // group index for one.
    return 1.0;
}

std::shared_ptr<TemporalPulse> CreateTemporalPulse(const ParamSet &params) {
    int nTimes = 0, nValues = 0;
    const Float *timesNs = params.FindFloat("pulsetimes", &nTimes);
    const Float *values = params.FindFloat("pulsevalues", &nValues);
    if (!timesNs && !values) {
        // Default triangular pulse on [0, 2] ns, peak 1 at 1 ns.
        return std::make_shared<TemporalPulse>(
            std::vector<double>{0.0, 1e-9, 2e-9},
            std::vector<double>{0.0, 1.0, 0.0});
    }
    if (!timesNs || !values) {
        MarkTransientConstructionError();
        Error(
            "Light pulse requires both \"pulsetimes\" and \"pulsevalues\"; "
            "only one of the two arrays was supplied.");
        return nullptr;
    }
    std::vector<double> t(nTimes), v(nValues);
    for (int i = 0; i < nTimes; ++i) t[i] = double(timesNs[i]) * 1e-9;
    for (int i = 0; i < nValues; ++i) v[i] = double(values[i]);
    try {
        return std::make_shared<TemporalPulse>(std::move(t), std::move(v));
    } catch (const std::invalid_argument &e) {
        MarkTransientConstructionError();
        Error("%s", e.what());
        return nullptr;
    }
}

// Scene construction runs single threaded; a plain flag is sufficient.
static bool g_transientConstructionError = false;

void MarkTransientConstructionError() { g_transientConstructionError = true; }

bool ConsumeTransientConstructionError() {
    bool failed = g_transientConstructionError;
    g_transientConstructionError = false;
    return failed;
}

bool TransientConstructionErrorOccurred() {
    return g_transientConstructionError;
}

// Transient transport local definitions
namespace {

// Accumulated state of one camera path.
struct TransientPath {
    Spectrum beta = Spectrum(1.f);
    Float etaScale = 1.f;
    // Physical propagation time from the camera along the real path, in
    // seconds.  Ray::time only carries the shutter time and is never used.
    double flightTime = 0.0;
    // Real geometric endpoint of the current ray.  SpawnRay offsets ray
    // origins to avoid self-intersection; that offset must not shorten the
    // physical distances, so timing always starts from this point.
    Point3f physicalOrigin;
};

// Add the propagation time of geometric segment [from, to] inside
// _medium_.  Distances use the real endpoint coordinates, so a non-unit ray
// direction cannot change the result.
inline void AddSegmentTime(TransientPath *path, const Point3f &from,
                           const Point3f &to, const Medium *medium,
                           double metersPerUnit) {
    const double length = double(Distance(from, to));
    path->flightTime +=
        length * MediumGroupIndex(medium) * metersPerUnit / SpeedOfLight;
}

// Result of walking a connection ray through media and material-less medium
// boundaries until an opaque surface is reached or the ray escapes.
struct ConnectionWalk {
    Spectrum transmittance = Spectrum(1.f);
    double seconds = 0.0;
    bool reachedMaterial = false;
    SurfaceInteraction endIsect;
};

// Connection walk shared by the light-sampling shadow connection and the
// BSDF/phase-sampling branch.  Transmittance uses the same Medium::Tr()
// calls as VisibilityTester::Tr(); the flight time of every medium segment
// (including segments split by empty medium boundaries) is accumulated
// between the real geometric endpoints.  _finalPoint_ is the light endpoint
// targeted by a shadow connection, or null for a scattered ray.
ConnectionWalk WalkConnection(Ray ray, const Point3f &start,
                              const Point3f *finalPoint, const Scene &scene,
                              Sampler &sampler, double metersPerUnit) {
    ConnectionWalk walk;
    Point3f segmentStart = start;
    while (true) {
        SurfaceInteraction hit;
        bool hitSurface = scene.Intersect(ray, &hit);

        // Beam transmittance over this ray segment.
        if (ray.medium) walk.transmittance *= ray.medium->Tr(ray, sampler);

        if (!hitSurface) {
            // Shadow rays terminate at the light endpoint; scattered rays
            // that escape have no finite emitter endpoint.
            if (finalPoint) {
                const double length = double(Distance(segmentStart,
                                                       *finalPoint));
                walk.seconds += length * MediumGroupIndex(ray.medium) *
                                metersPerUnit / SpeedOfLight;
            }
            return walk;
        }

        // Physical time up to the actual intersection point.
        const double length = double(Distance(segmentStart, hit.p));
        walk.seconds += length * MediumGroupIndex(ray.medium) *
                        metersPerUnit / SpeedOfLight;

        if (hit.primitive->GetMaterial() != nullptr) {
            walk.reachedMaterial = true;
            walk.endIsect = hit;
            return walk;
        }

        // Material-less medium boundary: switch medium only.  It consumes
        // no scattering depth and the accumulated optical path length is
        // retained.
        ray = finalPoint ? hit.SpawnRayTo(Interaction(*finalPoint, ray.time,
                                                      MediumInterface()))
                         : hit.SpawnRay(ray.d);
        segmentStart = hit.p;
    }
}

// Transient direct-lighting estimator.  The light-sampling and
// BSDF/phase-sampling MIS branches are fully separate: each keeps its own
// emitting endpoint, transmittance, emitter and flight time.  The light
// choice probability _lightChoosePdf_ mirrors UniformSampleOneLight().
void TransientEstimateDirect(
    const Interaction &it, const Point2f &uScattering, const Light &light,
    const Point2f &uLight, Float lightChoosePdf, const Scene &scene,
    Sampler &sampler, const TransientPath &path, double metersPerUnit,
    std::vector<TimedContribution> *contributions) {
    BxDFType bsdfFlags = BxDFType(BSDF_ALL & ~BSDF_SPECULAR);

    // ---- Light-sampling MIS branch -------------------------------------
    Vector3f wi;
    Float lightPdf = 0, scatteringPdf = 0;
    VisibilityTester visibility;
    Spectrum Li = light.Sample_Li(it, uLight, &wi, &lightPdf, &visibility);
    if (lightPdf > 0 && !Li.IsBlack()) {
        Spectrum f;
        if (it.IsSurfaceInteraction()) {
            const SurfaceInteraction &isect =
                (const SurfaceInteraction &)it;
            f = isect.bsdf->f(isect.wo, wi, bsdfFlags) *
                AbsDot(wi, isect.shading.n);
            scatteringPdf = isect.bsdf->Pdf(isect.wo, wi, bsdfFlags);
        } else {
            const MediumInteraction &mi = (const MediumInteraction &)it;
            Float p = mi.phase->p(mi.wo, wi);
            f = Spectrum(p);
            scatteringPdf = p;
        }
        if (!f.IsBlack()) {
            const Interaction &p0 = visibility.P0();
            const Interaction &p1 = visibility.P1();
            Ray shadowRay = p0.SpawnRayTo(p1);
            ConnectionWalk walk = WalkConnection(
                shadowRay, p0.p, &p1.p, scene, sampler, metersPerUnit);

            // An occluded connection contributes nothing.
            if (!walk.reachedMaterial) {
                Li *= walk.transmittance;
                if (!Li.IsBlack()) {
                    Float weight = 1.f;
                    if (!IsDeltaLight(light.flags))
                        weight = PowerHeuristic(1, lightPdf, 1,
                                                scatteringPdf);
                    Spectrum value = path.beta * f * Li * weight /
                                     (lightPdf * lightChoosePdf);
                    contributions->emplace_back(
                        value, path.flightTime + walk.seconds, &light);
                }
            }
        }
    }

    // ---- BSDF / phase-sampling MIS branch -------------------------------
    if (IsDeltaLight(light.flags)) return;

    Spectrum f;
    bool sampledSpecular = false;
    if (it.IsSurfaceInteraction()) {
        BxDFType sampledType;
        const SurfaceInteraction &isect = (const SurfaceInteraction &)it;
        f = isect.bsdf->Sample_f(isect.wo, &wi, uScattering, &scatteringPdf,
                                 bsdfFlags, &sampledType);
        f *= AbsDot(wi, isect.shading.n);
        sampledSpecular = (sampledType & BSDF_SPECULAR) != 0;
    } else {
        const MediumInteraction &mi = (const MediumInteraction &)it;
        Float p = mi.phase->Sample_p(mi.wo, &wi, uScattering);
        f = Spectrum(p);
        scatteringPdf = p;
    }
    if (f.IsBlack() || scatteringPdf <= 0) return;

    Float weight = 1.f;
    if (!sampledSpecular) {
        lightPdf = light.Pdf_Li(it, wi);
        if (lightPdf == 0) return;
        weight = PowerHeuristic(1, scatteringPdf, 1, lightPdf);
    }

    Ray connRay = it.SpawnRay(wi);
    ConnectionWalk walk = WalkConnection(connRay, it.p, nullptr, scene,
                                         sampler, metersPerUnit);
    // An escaped ray has no finite emitter endpoint; an opaque surface that
    // is not this light blocks the connection.
    if (!walk.reachedMaterial) return;

    Spectrum Ld(0.f);
    const AreaLight *area = walk.endIsect.primitive->GetAreaLight();
    if (area == &light) Ld = walk.endIsect.Le(-wi);
    if (!Ld.IsBlack()) {
        Spectrum value = path.beta * f * Ld * walk.transmittance * weight /
                         (scatteringPdf * lightChoosePdf);
        contributions->emplace_back(value,
                                    path.flightTime + walk.seconds, &light);
    }
}

}  // namespace

std::vector<TimedContribution> TraceTransient(
    const RayDifferential &r, const Scene &scene, Sampler &sampler,
    MemoryArena &arena, int maxDepth, Float rrThreshold,
    double metersPerUnit) {
    ProfilePhase p(Prof::SamplerIntegratorLi);
    std::vector<TimedContribution> contributions;
    RayDifferential ray(r);
    TransientPath path;
    path.physicalOrigin = ray.o;
    bool specularBounce = false;
    int bounces;

    for (bounces = 0;; ++bounces) {
        // Intersect _ray_ with the scene.
        SurfaceInteraction isect;
        bool foundIntersection = scene.Intersect(ray, &isect);

        // Sample the participating medium with the same routine and random
        // number stream as VolPathIntegrator, so throughput and the sample
        // sequence match.
        MediumInteraction mi;
        if (ray.medium)
            path.beta *= ray.medium->Sample(ray, sampler, arena, &mi);
        if (path.beta.IsBlack()) break;

        if (mi.IsValid()) {
            // Physical segment from the previous vertex to the scattering
            // event, measured between real geometric endpoints.
            AddSegmentTime(&path, path.physicalOrigin, mi.p, ray.medium,
                           metersPerUnit);

            if (bounces >= maxDepth) break;
            ++transientVolumeInteractions;

            // Uniformly choose one light; no time random numbers are added.
            const int nLights = int(scene.lights.size());
            if (nLights > 0) {
                int lightNum =
                    std::min((int)(sampler.Get1D() * nLights), nLights - 1);
                const Light &light = *scene.lights[lightNum];
                Point2f uLight = sampler.Get2D();
                Point2f uScattering = sampler.Get2D();
                TransientEstimateDirect(
                    mi, uScattering, light, uLight, Float(1) / nLights,
                    scene, sampler, path, metersPerUnit, &contributions);
            }

            Vector3f wo = -ray.d, wi;
            mi.phase->Sample_p(wo, &wi, sampler.Get2D());
            ray = mi.SpawnRay(wi);
            path.physicalOrigin = mi.p;
            specularBounce = false;
        } else {
            ++transientSurfaceInteractions;

            // Physical segment up to the real surface point.  The ray
            // origin may be self-intersection-offset; physicalOrigin is not.
            if (foundIntersection)
                AddSegmentTime(&path, path.physicalOrigin, isect.p, ray.medium,
                               metersPerUnit);

            // Emitted light at a primary or specular continuation hit.  The
            // area-light endpoint and its true flight time are retained.
            if (bounces == 0 || specularBounce) {
                if (foundIntersection) {
                    Spectrum Le = isect.Le(-ray.d);
                    if (!Le.IsBlack()) {
                        contributions.emplace_back(
                            path.beta * Le, path.flightTime,
                            isect.primitive->GetAreaLight());
                    }
                }
                // Escaped rays could only reach infinite lights, which are
                // rejected before rendering.
            }

            if (!foundIntersection || bounces >= maxDepth) break;

            isect.ComputeScatteringFunctions(ray, arena, true);
            if (!isect.bsdf) {
                // Material-less medium boundary: switch medium without
                // consuming scattering depth; keep the optical path length.
                ray = isect.SpawnRay(ray.d);
                path.physicalOrigin = isect.p;
                bounces--;
                continue;
            }

            // Uniformly choose one light for direct lighting.
            const int nLights = int(scene.lights.size());
            if (nLights > 0) {
                int lightNum =
                    std::min((int)(sampler.Get1D() * nLights), nLights - 1);
                const Light &light = *scene.lights[lightNum];
                Point2f uLight = sampler.Get2D();
                Point2f uScattering = sampler.Get2D();
                TransientEstimateDirect(
                    isect, uScattering, light, uLight, Float(1) / nLights,
                    scene, sampler, path, metersPerUnit, &contributions);
            }

            // Sample the BSDF for the continuation.
            Vector3f wo = -ray.d, wi;
            Float pdf;
            BxDFType flags;
            Spectrum f = isect.bsdf->Sample_f(wo, &wi, sampler.Get2D(), &pdf,
                                              BSDF_ALL, &flags);
            if (f.IsBlack() || pdf == 0.f) break;
            path.beta *= f * AbsDot(wi, isect.shading.n) / pdf;
            DCHECK(std::isinf(path.beta.y()) == false);
            specularBounce = (flags & BSDF_SPECULAR) != 0;
            if ((flags & BSDF_SPECULAR) && (flags & BSDF_TRANSMISSION)) {
                Float eta = isect.bsdf->eta;
                path.etaScale *=
                    (Dot(wo, isect.n) > 0) ? (eta * eta) : 1 / (eta * eta);
            }
            ray = isect.SpawnRay(wi);
            path.physicalOrigin = isect.p;

            // BSSRDFs are rejected at scene validation; never silently drop
            // their timing if one is encountered anyway.
            if (isect.bssrdf && (flags & BSDF_TRANSMISSION))
                Error("BSSRDF transport is not supported by transientvolpath.");
        }

        // Russian roulette with the same threshold and throughput rule as
        // VolPathIntegrator.
        Spectrum rrBeta = path.beta * path.etaScale;
        if (rrBeta.MaxComponentValue() < rrThreshold && bounces > 3) {
            Float q = std::max((Float).05, 1 - rrBeta.MaxComponentValue());
            if (sampler.Get1D() < q) break;
            path.beta /= 1 - q;
            DCHECK(std::isinf(path.beta.y()) == false);
        }
    }
    ReportValue(transientPathLength, bounces);
    return contributions;
}

bool ValidateTransientScene(const Scene &scene, bool sampledSpectrum,
                            std::string *failureReason) {
    auto fail = [&](const char *message) -> bool {
        if (failureReason) *failureReason = message;
        Error("%s", message);
        return false;
    };

#ifdef PBRT_SAMPLED_SPECTRUM
    return fail(
        "transientvolpath requires an RGB (non-SampledSpectrum) build.");
#else
    if (sampledSpectrum)
        return fail(
            "transientvolpath requires an RGB (non-SampledSpectrum) build.");
#endif

    if (!scene.infiniteLights.empty())
        return fail(
            "transientvolpath does not support infinite or environment "
            "lights; use a finite PointLight or a constant "
            "DiffuseAreaLight.");

    for (const auto &light : scene.lights) {
        if (!light)
            return fail("transientvolpath found a null light in the scene.");
        if (dynamic_cast<const PointLight *>(light.get())) continue;
        if (dynamic_cast<const DiffuseAreaLight *>(light.get())) continue;
        return fail(
            "transientvolpath only supports finite PointLight and constant "
            "DiffuseAreaLight sources; other light types are rejected.");
    }
    return true;
}

// Transient film local definitions
namespace {

constexpr double NanoSecondsPerSecond = 1e9;

// Double-precision copy of the XYZ->RGB matrix used by RGBSpectrum, so the
// in-memory readout and the CSV keep identical values.
inline void XYZToRGBDouble(const double xyz[3], double rgb[3]) {
    rgb[0] = 3.240479 * xyz[0] - 1.537150 * xyz[1] - 0.498535 * xyz[2];
    rgb[1] = -0.969256 * xyz[0] + 1.875991 * xyz[1] + 0.041556 * xyz[2];
    rgb[2] = 0.055648 * xyz[0] - 0.204043 * xyz[1] + 1.057311 * xyz[2];
}

// One film tile carrying per-bin, per-pixel XYZ energy plus a single spatial
// filter denominator per pixel.
class TransientFilmTile : public FilmTile {
  public:
    TransientFilmTile(const Bounds2i &pixelBounds, const Vector2f &filterRadius,
                      const Float *filterTable, int filterTableSize,
                      int bins)
        : FilmTile(pixelBounds, filterRadius, filterTable, filterTableSize,
                   Infinity),
          bins(bins),
          binEnergy(size_t(pixelBounds.Area()) * bins * 3, 0.0),
          weights(size_t(pixelBounds.Area()), 0.0) {
        binSeconds.resize(bins + 1);
    }

    void AddTransientSample(
        const Point2f &pFilm,
        const std::vector<TimedContribution> &contribs,
        Float rayWeight) override {
        ProfilePhase _(Prof::AddFilmSample);
        const Vector2f radius = TileFilterRadius();
        Point2f pFilmDiscrete = pFilm - Vector2f(0.5f, 0.5f);
        Point2i p0 = (Point2i)Ceil(pFilmDiscrete - radius);
        Point2i p1 = (Point2i)Floor(pFilmDiscrete + radius) + Point2i(1, 1);
        const Bounds2i bounds = GetPixelBounds();
        p0 = Max(p0, bounds.pMin);
        p1 = Min(p1, bounds.pMax);
        const int tableSize = TileFilterTableSize();
        const Float *table = TileFilterTable();
        const Vector2f invRadius = TileInvFilterRadius();

        int *ifx = ALLOCA(int, p1.x - p0.x);
        for (int x = p0.x; x < p1.x; ++x) {
            Float fx = std::abs((x - pFilmDiscrete.x) * invRadius.x *
                                tableSize);
            ifx[x - p0.x] = std::min((int)std::floor(fx), tableSize - 1);
        }
        int *ify = ALLOCA(int, p1.y - p0.y);
        for (int y = p0.y; y < p1.y; ++y) {
            Float fy = std::abs((y - pFilmDiscrete.y) * invRadius.y *
                                tableSize);
            ify[y - p0.y] = std::min((int)std::floor(fy), tableSize - 1);
        }

        for (int y = p0.y; y < p1.y; ++y) {
            for (int x = p0.x; x < p1.x; ++x) {
                Float filterWeight = table[ify[y - p0.y] * tableSize +
                                           ifx[x - p0.x]];
                const int pixOffset = TilePixelOffset(Point2i(x, y));
                // Spatial filter denominator: exactly once per covered
                // pixel per camera sample, independent of the number of
                // contributions / bins, out-of-window samples, or weight.
                weights[pixOffset] += double(filterWeight);
                if (rayWeight == 0.f) continue;

                for (const TimedContribution &c : contribs) {
                    if (!c.emitter || !c.emitter->pulse ||
                        c.value.IsBlack())
                        continue;
                    Float rgbF[3];
                    c.value.ToRGB(rgbF);
                    Float xyzF[3];
                    RGBToXYZ(rgbF, xyzF);
                    for (int b = 0; b < bins; ++b) {
                        // Mass of the emitter pulse over this bin after the
                        // contribution's physical flight-time shift.
                        const double mass =
                            c.emitter->pulse->ShiftedIntegral(
                                binSeconds[b], binSeconds[b + 1],
                                c.flightTime);
                        if (mass == 0.0) continue;
                        double *slot =
                            &binEnergy[(size_t(pixOffset) * bins + b) * 3];
                        const double w =
                            double(rayWeight) * double(filterWeight) * mass;
                        slot[0] += double(xyzF[0]) * w;
                        slot[1] += double(xyzF[1]) * w;
                        slot[2] += double(xyzF[2]) * w;
                    }
                }
            }
        }
    }

    int Bins() const { return bins; }
    int PixelOffsetFor(const Point2i &p) const { return TilePixelOffset(p); }
    const double *PixelEnergy(int pixelOffset) const {
        return &binEnergy[size_t(pixelOffset) * bins * 3];
    }
    double PixelWeight(int pixelOffset) const { return weights[pixelOffset]; }
    Bounds2i Bounds() const { return GetPixelBounds(); }

    std::vector<double> binSeconds;

  private:
    const int bins;
    std::vector<double> binEnergy;
    std::vector<double> weights;
};

}  // namespace

TransientFilm::TransientFilm(const Point2i &resolution,
                             const Bounds2f &cropWindow,
                             std::unique_ptr<Filter> filter, Float diagonal,
                             const std::string &filename, Float scale,
                             int bins, Float tStartNs, Float binWidthNs)
    : Film(resolution, cropWindow, std::move(filter), diagonal, filename,
           scale, Infinity),
      bins(bins),
      tStartNs(tStartNs),
      binWidthNs(binWidthNs) {
    const int area = croppedPixelBounds.Area();
    const size_t channels = size_t(bins) * 3;
    if (area < 0 || size_t(area) > size_t(-1) /
                               (channels * sizeof(double))) {
        MarkTransientConstructionError();
        Error("Transient film storage size overflows for %d pixels and %d "
              "bins.", area, bins);
        return;
    }
    binXYZ.reset(new double[size_t(area) * channels]());
    filterWeights.reset(new double[area]());
}

std::unique_ptr<FilmTile> TransientFilm::GetFilmTile(
    const Bounds2i &sampleBounds) {
    Vector2f halfPixel = Vector2f(0.5f, 0.5f);
    Bounds2f floatBounds = (Bounds2f)sampleBounds;
    Point2i p0 = (Point2i)Ceil(floatBounds.pMin - halfPixel - filter->radius);
    Point2i p1 = (Point2i)Floor(floatBounds.pMax - halfPixel + filter->radius) +
                 Point2i(1, 1);
    Bounds2i tilePixelBounds = Intersect(Bounds2i(p0, p1), croppedPixelBounds);
    std::unique_ptr<TransientFilmTile> tile(new TransientFilmTile(
        tilePixelBounds, filter->radius, FilterTableValues(),
        FilterTableWidthValue(), bins));
    for (int b = 0; b <= bins; ++b)
        tile->binSeconds[b] =
            (double(tStartNs) + double(b) * double(binWidthNs)) /
            NanoSecondsPerSecond;
    return std::unique_ptr<FilmTile>(tile.release());
}

void TransientFilm::MergeFilmTile(std::unique_ptr<FilmTile> tile) {
    ProfilePhase profile(Prof::MergeFilmTile);
    TransientFilmTile *tt = dynamic_cast<TransientFilmTile *>(tile.get());
    CHECK(tt != nullptr);
    std::lock_guard<std::mutex> lock(FilmMutex());
    const int width = croppedPixelBounds.pMax.x - croppedPixelBounds.pMin.x;
    for (Point2i pixel : tt->Bounds()) {
        const int tileOffset = tt->PixelOffsetFor(pixel);
        const int filmOffset =
            (pixel.x - croppedPixelBounds.pMin.x) +
            (pixel.y - croppedPixelBounds.pMin.y) * width;
        const double *src = tt->PixelEnergy(tileOffset);
        double *dst = &binXYZ[size_t(filmOffset) * bins * 3];
        for (int i = 0; i < 3 * bins; ++i) dst[i] += src[i];
        filterWeights[filmOffset] += tt->PixelWeight(tileOffset);
    }
}

std::vector<std::array<double, 3>> TransientFilm::GetTransientRGB(
    const Point2i &p) const {
    std::vector<std::array<double, 3>> rgb(bins);
    for (auto &c : rgb) c = {0.0, 0.0, 0.0};
    if (!InsideExclusive(p, croppedPixelBounds) || !binXYZ) return rgb;
    const int offset = PixelOffset(p);
    const double invWt = filterWeights[offset] != 0.0
                             ? 1.0 / filterWeights[offset]
                             : 0.0;
    const double *energy = &binXYZ[size_t(offset) * bins * 3];
    for (int b = 0; b < bins; ++b) {
        double xyz[3] = {energy[3 * b], energy[3 * b + 1],
                         energy[3 * b + 2]};
        for (double &c : xyz) c *= invWt * double(FilmScale());
        double out[3];
        XYZToRGBDouble(xyz, out);
        rgb[b] = {std::max(0.0, out[0]), std::max(0.0, out[1]),
                  std::max(0.0, out[2])};
    }
    return rgb;
}

void TransientFilm::WriteImage(Float splatScale) {
    // Splatting is not used by the transient integrator; the argument is
    // retained to match the Film interface.
    (void)splatScale;
    std::ofstream out(filename, std::ios::trunc);
    if (!out.good()) {
        Error("Unable to open transient film output file \"%s\".",
              filename.c_str());
        return;
    }
    out << "x,y,bin,t0_ns,t1_ns,r,g,b\n";
    // max_digits10 precision round-trips a double, so the CSV values are
    // identical to the in-memory GetTransientRGB readout.
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    // Rows are emitted in increasing y, then x, then bin order.
    for (int y = croppedPixelBounds.pMin.y; y < croppedPixelBounds.pMax.y;
         ++y) {
        for (int x = croppedPixelBounds.pMin.x; x < croppedPixelBounds.pMax.x;
             ++x) {
            const std::vector<std::array<double, 3>> rgb =
                GetTransientRGB(Point2i(x, y));
            for (int b = 0; b < bins; ++b) {
                const double t0 =
                    double(tStartNs) + double(b) * double(binWidthNs);
                const double t1 =
                    double(tStartNs) + double(b + 1) * double(binWidthNs);
                out << x << ',' << y << ',' << b << ',' << t0 << ',' << t1
                    << ',' << rgb[b][0] << ',' << rgb[b][1] << ','
                    << rgb[b][2] << '\n';
            }
        }
    }
}

void TransientFilm::Clear() {
    if (!binXYZ) return;
    const int area = croppedPixelBounds.Area();
    std::fill(binXYZ.get(), binXYZ.get() + size_t(area) * bins * 3, 0.0);
    std::fill(filterWeights.get(), filterWeights.get() + area, 0.0);
}

Film *CreateTransientFilm(const ParamSet &params,
                          std::unique_ptr<Filter> filter) {
    std::string filename;
    if (PbrtOptions.imageFile != "") {
        filename = PbrtOptions.imageFile;
        std::string paramsFilename = params.FindOneString("filename", "");
        if (paramsFilename != "")
            Warning(
                "Output filename supplied on command line, \"%s\" is "
                "overriding filename provided in scene description file, "
                "\"%s\".",
                PbrtOptions.imageFile.c_str(), paramsFilename.c_str());
    } else {
        filename = params.FindOneString("filename", "transient.csv");
    }

    int xres = params.FindOneInt("xresolution", 1280);
    int yres = params.FindOneInt("yresolution", 720);
    if (PbrtOptions.quickRender) {
        xres = std::max(1, xres / 4);
        yres = std::max(1, yres / 4);
    }
    Bounds2f crop;
    int cwi;
    const Float *cr = params.FindFloat("cropwindow", &cwi);
    if (cr && cwi == 4) {
        crop.pMin.x = Clamp(std::min(cr[0], cr[1]), 0.f, 1.f);
        crop.pMax.x = Clamp(std::max(cr[0], cr[1]), 0.f, 1.f);
        crop.pMin.y = Clamp(std::min(cr[2], cr[3]), 0.f, 1.f);
        crop.pMax.y = Clamp(std::max(cr[2], cr[3]), 0.f, 1.f);
    } else if (cr) {
        Error("%d values supplied for \"cropwindow\". Expected 4.", cwi);
        MarkTransientConstructionError();
        return nullptr;
    } else {
        crop = Bounds2f(Point2f(Clamp(PbrtOptions.cropWindow[0][0], 0, 1),
                                Clamp(PbrtOptions.cropWindow[1][0], 0, 1)),
                        Point2f(Clamp(PbrtOptions.cropWindow[0][1], 0, 1),
                                Clamp(PbrtOptions.cropWindow[1][1], 0, 1)));
    }

    Float scale = params.FindOneFloat("scale", 1.);
    Float diagonal = params.FindOneFloat("diagonal", 35.);

    // Per-bin luminance clamping is intentionally unsupported.
    if (std::isfinite(params.FindOneFloat("maxsampleluminance", Infinity))) {
        Error(
            "Transient film does not support \"maxsampleluminance\"; remove "
            "the parameter.");
        MarkTransientConstructionError();
        return nullptr;
    }

    int bins = params.FindOneInt("bins", 64);
    if (bins < 1 || bins > 4096) {
        Error("Transient film \"bins\" must be in [1, 4096]; got %d.", bins);
        MarkTransientConstructionError();
        return nullptr;
    }
    Float tStart = params.FindOneFloat("tstart", 0.f);
    Float binWidth = params.FindOneFloat("binwidth", 1.f);
    if (!std::isfinite(tStart) || tStart < 0.f) {
        Error("Transient film \"tstart\" must be finite and non-negative.");
        MarkTransientConstructionError();
        return nullptr;
    }
    if (!std::isfinite(binWidth) || binWidth <= 0.f) {
        Error("Transient film \"binwidth\" must be finite and positive.");
        MarkTransientConstructionError();
        return nullptr;
    }
    // The final bin edge must be finite; this also rejects end-time overflow.
    if (!std::isfinite(tStart + Float(bins) * binWidth)) {
        Error(
            "Transient film end time is not finite; reduce \"bins\" or "
            "\"binwidth\".");
        MarkTransientConstructionError();
        return nullptr;
    }

    return new TransientFilm(Point2i(xres, yres), crop, std::move(filter),
                             diagonal, filename, scale, bins, tStart,
                             binWidth);
}

}  // namespace pbrt
