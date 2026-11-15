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

// media/instancegrid.cpp*
#include "media/instancegrid.h"
#include "interaction.h"
#include "paramset.h"
#include "sampler.h"
#include "stats.h"
#include "stringprint.h"
#include <cmath>
#include <limits>

namespace pbrt {

STAT_RATIO("Media/InstanceGrid steps per Tr() call", nInstanceGridTrSteps,
           nInstanceGridTrCalls);

namespace {

// The density grid lives in the normalized cube [0,1]^3.  An affine
// transform that is finite, has an affine last row, and has a non-singular
// upper-left 3x3 block is required both to map world rays into the grid and
// to map grid points back out to the world.
bool ValidateAffineTransform(const Transform &t, const char *what,
                             std::string *error) {
    const Float(*m)[4] = t.GetMatrix().m;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!std::isfinite(m[i][j])) {
                if (error)
                    *error = StringPrintf("%s transform has a non-finite "
                                          "entry at row %d, column %d",
                                          what, i, j);
                return false;
            }
    // Require an affine last row; projective transforms cannot be inverted
    // pointwise inside the volume.
    if (m[3][0] != 0 || m[3][1] != 0 || m[3][2] != 0 || m[3][3] != 1) {
        if (error)
            *error = StringPrintf("%s transform is not affine", what);
        return false;
    }
    // Determinant of the linear block, computed in double to avoid overflow
    // for dimensionally large (but still finite) transforms.
    double det =
        double(m[0][0]) * (double(m[1][1]) * double(m[2][2]) -
                           double(m[1][2]) * double(m[2][1])) -
        double(m[0][1]) * (double(m[1][0]) * double(m[2][2]) -
                           double(m[1][2]) * double(m[2][0])) +
        double(m[0][2]) * (double(m[1][0]) * double(m[2][1]) -
                           double(m[1][1]) * double(m[2][0]));
    double scale = 1;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            scale = std::max(scale, std::fabs(double(m[i][j])));
    // Treat the matrix as singular when its determinant is tiny relative to
    // the cube of its largest entry.
    double relativeDet = std::fabs(det) / (scale * scale * scale);
    if (!(relativeDet > 1e-12)) {
        if (error)
            *error = StringPrintf("%s transform is singular (determinant %g)",
                                  what, det);
        return false;
    }
    return true;
}

}  // namespace

bool ValidateInstanceTransform(const Transform &prototypeToWorld,
                               std::string *error) {
    return ValidateAffineTransform(prototypeToWorld, "instancegrid", error);
}

// InstanceGridDensity Method Definitions
std::shared_ptr<const InstanceGridDensity> InstanceGridDensity::Create(
    const Spectrum &sigma_a, const Spectrum &sigma_s, Float g, int nx, int ny,
    int nz, const Transform &densityToPrototype, const Float *density,
    std::string *error) {
    if (!density) {
        if (error) *error = "no density values provided";
        return nullptr;
    }
    if (nx <= 0 || ny <= 0 || nz <= 0) {
        if (error)
            *error = StringPrintf("invalid grid dimensions %d x %d x %d", nx,
                                  ny, nz);
        return nullptr;
    }
    // Detect dimension/product overflow before allocating anything.  The
    // stepwise product guard avoids overflowing int64 for large dimensions.
    int64_t count = 1;
    int64_t dims[3] = {nx, ny, nz};
    for (int i = 0; i < 3; ++i) {
        if (dims[i] > 0 &&
            count > int64_t(std::numeric_limits<int>::max()) / dims[i]) {
            count = -1;
            break;
        }
        count *= dims[i];
    }
    if (count <= 0 ||
        size_t(count) >
            size_t(std::numeric_limits<size_t>::max() / sizeof(Float))) {
        if (error)
            *error = StringPrintf("grid dimensions %d x %d x %d overflow",
                                  nx, ny, nz);
        return nullptr;
    }
    for (int64_t i = 0; i < count; ++i) {
        Float d = density[i];
        if (!std::isfinite(d) || d < 0) {
            if (error)
                *error = StringPrintf(
                    "density value %d is invalid (must be finite and "
                    "non-negative, got %f)",
                    int(i), d);
            return nullptr;
        }
    }
    if (!ValidateAffineTransform(densityToPrototype, "density-to-prototype",
                                 error))
        return nullptr;
    // The delta/ratio tracking majorant requires a strictly positive
    // extinction and maximum density.
    Spectrum sigma_t_spec = sigma_a + sigma_s;
    Float sigma_t = sigma_t_spec[0];
    if (!std::isfinite(sigma_t) || sigma_t <= 0) {
        if (error)
            *error = StringPrintf(
                "extinction coefficient must be finite and positive (got %f)",
                sigma_t);
        return nullptr;
    }
    if (Spectrum(sigma_t) != sigma_t_spec) {
        if (error)
            *error =
                "instancegrid requires a spectrally uniform attenuation "
                "coefficient";
        return nullptr;
    }
    Float maxDensity = 0;
    for (int64_t i = 0; i < count; ++i)
        maxDensity = std::max(maxDensity, density[i]);
    if (!(maxDensity > 0)) {
        if (error)
            *error = "density grid contains no positive values";
        return nullptr;
    }
    return std::shared_ptr<const InstanceGridDensity>(
        new InstanceGridDensity(sigma_a, sigma_s, g, nx, ny, nz,
                                densityToPrototype, density));
}

InstanceGridDensity::InstanceGridDensity(const Spectrum &sigma_a,
                                         const Spectrum &sigma_s, Float g,
                                         int nx, int ny, int nz,
                                         const Transform &densityToPrototype,
                                         const Float *densityValues)
    : sigma_a(sigma_a),
      sigma_s(sigma_s),
      g(g),
      nx(nx),
      ny(ny),
      nz(nz),
      densityToPrototype(densityToPrototype),
      density(new Float[nx * ny * nz]) {
    memcpy(density.get(), densityValues, sizeof(Float) * nx * ny * nz);
    sigma_t = (sigma_a + sigma_s)[0];
    maxDensity = 0;
    for (int i = 0; i < nx * ny * nz; ++i)
        maxDensity = std::max(maxDensity, density[i]);
    invMaxDensity = 1 / maxDensity;
}

Float InstanceGridDensity::D(const Point3i &p) const {
    Bounds3i sampleBounds(Point3i(0, 0, 0), Point3i(nx, ny, nz));
    if (!InsideExclusive(p, sampleBounds)) return 0;
    return density[(p.z * ny + p.y) * nx + p.x];
}

Float InstanceGridDensity::Density(const Point3f &p) const {
    // Compute voxel coordinates and offsets for _p_; this is the same
    // trilinear interpolation used by GridDensityMedium.
    Point3f pSamples(p.x * nx - .5f, p.y * ny - .5f, p.z * nz - .5f);
    Point3i pi = (Point3i)Floor(pSamples);
    Vector3f d = pSamples - (Point3f)pi;

    Float d00 = Lerp(d.x, D(pi), D(pi + Vector3i(1, 0, 0)));
    Float d10 = Lerp(d.x, D(pi + Vector3i(0, 1, 0)), D(pi + Vector3i(1, 1, 0)));
    Float d01 = Lerp(d.x, D(pi + Vector3i(0, 0, 1)), D(pi + Vector3i(1, 0, 1)));
    Float d11 = Lerp(d.x, D(pi + Vector3i(0, 1, 1)), D(pi + Vector3i(1, 1, 1)));
    Float d0 = Lerp(d.y, d00, d10);
    Float d1 = Lerp(d.y, d01, d11);
    return Lerp(d.z, d0, d1);
}

// InstanceGridMedium Method Definitions
InstanceGridMedium::InstanceGridMedium(
    std::shared_ptr<const InstanceGridDensity> grid,
    const Transform &densityToWorld, bool isPrototype,
    const std::string &prototypeName)
    : grid(std::move(grid)),
      densityToWorld(densityToWorld),
      worldToDensity(Inverse(densityToWorld)),
      isPrototype(isPrototype),
      prototypeName(prototypeName) {}

std::shared_ptr<InstanceGridMedium> InstanceGridMedium::CreatePrototype(
    std::shared_ptr<const InstanceGridDensity> grid,
    const std::string &prototypeName) {
    return std::shared_ptr<InstanceGridMedium>(new InstanceGridMedium(
        std::move(grid), Transform(), true, prototypeName));
}

std::shared_ptr<InstanceGridMedium> InstanceGridMedium::CreatePlacement(
    std::shared_ptr<const InstanceGridDensity> grid,
    const Transform &prototypeToWorld, const std::string &prototypeName,
    std::string *error) {
    if (!ValidateAffineTransform(prototypeToWorld, "instance placement",
                                 error))
        return nullptr;
    Transform densityToWorld = prototypeToWorld * grid->DensityToPrototype();
    if (!ValidateAffineTransform(densityToWorld, "density-to-world", error))
        return nullptr;
    return std::shared_ptr<InstanceGridMedium>(new InstanceGridMedium(
        std::move(grid), densityToWorld, false, prototypeName));
}

// Maps a world ray into the density cube while keeping _t_ measured in world
// distance.  This is what makes non-uniform scale and shear behave correctly:
// the *direction* carries the local stretch (so a ray crossing a stretched
// smoke traverses more world length and accumulates more optical thickness)
// while sigma_t stays in units of inverse world length and is never scaled.
bool InstanceGridMedium::OverlapWorldRay(const Ray &rWorld, Ray *rDistance,
                                         Ray *rDensity, Float *t0,
                                         Float *t1) const {
    Float dLength = rWorld.d.Length();
    if (!(dLength > 0) || !std::isfinite(dLength)) {
        // Degenerate directions cannot be integrated; report empty overlap.
        return false;
    }
    Vector3f dWorld = rWorld.d / dLength;
    Float tMaxWorld = rWorld.tMax * dLength;
    *rDistance = Ray(rWorld.o, dWorld, tMaxWorld, rWorld.time, rWorld.medium);
    Point3f oDensity = worldToDensity(rWorld.o);
    Vector3f dDensity = worldToDensity(dWorld);
    *rDensity = Ray(oDensity, dDensity, tMaxWorld, rWorld.time,
                    rWorld.medium);
    const Bounds3f bounds(Point3f(0, 0, 0), Point3f(1, 1, 1));
    if (!bounds.IntersectP(*rDensity, t0, t1)) return false;
    if (*t1 < 0) return false;
    *t0 = std::max(*t0, (Float)0);
    return *t0 <= *t1;
}

Spectrum InstanceGridMedium::Sample(const Ray &rWorld, Sampler &sampler,
                                    MemoryArena &arena,
                                    MediumInteraction *mi) const {
    ProfilePhase _(Prof::MediumSample);
    Ray rDistance, rDensity;
    Float t0, t1;
    if (!OverlapWorldRay(rWorld, &rDistance, &rDensity, &t0, &t1))
        return Spectrum(1.f);

    Float sigma_t = grid->SigmaT();
    Float invMaxDensity = grid->InvMaxDensity();
    // Delta-track in world-distance units.
    Float t = t0;
    while (true) {
        t -= std::log(1 - sampler.Get1D()) * invMaxDensity / sigma_t;
        if (t >= t1) break;
        if (grid->Density(rDensity(t)) * invMaxDensity > sampler.Get1D()) {
            PhaseFunction *phase = ARENA_ALLOC(arena, HenyeyGreenstein)(
                grid->G());
            Point3f pWorld = rDistance(t);
            *mi = MediumInteraction(pWorld, -rDistance.d, rWorld.time, this,
                                    phase);
            return grid->SigmaS() / Spectrum(sigma_t);
        }
    }
    return Spectrum(1.f);
}

Spectrum InstanceGridMedium::Tr(const Ray &rWorld, Sampler &sampler) const {
    ProfilePhase _(Prof::MediumTr);
    ++nInstanceGridTrCalls;
    Ray rDistance, rDensity;
    Float t0, t1;
    if (!OverlapWorldRay(rWorld, &rDistance, &rDensity, &t0, &t1))
        return Spectrum(1.f);

    Float sigma_t = grid->SigmaT();
    Float invMaxDensity = grid->InvMaxDensity();
    // Ratio-track in world-distance units.
    Float Tr = 1, t = t0;
    while (true) {
        ++nInstanceGridTrSteps;
        t -= std::log(1 - sampler.Get1D()) * invMaxDensity / sigma_t;
        if (t >= t1) break;
        Float density = grid->Density(rDensity(t));
        Tr *= 1 - std::max((Float)0, density * invMaxDensity);
        // Russian roulette once transmittance becomes small, as in
        // GridDensityMedium.
        const Float rrThreshold = .1;
        if (Tr < rrThreshold) {
            Float q = std::max((Float).05, 1 - Tr);
            if (sampler.Get1D() < q) return 0;
            Tr /= 1 - q;
        }
    }
    return Spectrum(Tr);
}

// InstanceGridMedium Remap Definitions
namespace {

class InstanceGridMediumRemap : public MediumRemap {
  public:
    InstanceGridMediumRemap(
        std::vector<std::shared_ptr<InstanceGridMedium>> placements)
        : placements(std::move(placements)) {}

    void Add(const Medium *prototype, const Medium *placement) {
        mapping[prototype] = placement;
    }

    const Medium *Remap(const Medium *medium) const {
        auto iter = mapping.find(medium);
        return iter == mapping.end() ? medium : iter->second;
    }

  private:
    // Keeps the placement views alive for as long as any primitive that
    // stored their pointers is alive.
    std::vector<std::shared_ptr<InstanceGridMedium>> placements;
    std::map<const Medium *, const Medium *> mapping;
};

}  // namespace

std::vector<std::shared_ptr<InstanceGridMedium>> CreateInstancePlacementViews(
    const std::vector<std::shared_ptr<InstanceGridMedium>> &prototypeMedia,
    const Transform &prototypeToWorld, std::string *error) {
    std::vector<std::shared_ptr<InstanceGridMedium>> views;
    views.reserve(prototypeMedia.size());
    for (const auto &prototype : prototypeMedia) {
        if (!prototype) continue;
        std::shared_ptr<InstanceGridMedium> view =
            InstanceGridMedium::CreatePlacement(
                prototype->GridShared(), prototypeToWorld,
                prototype->PrototypeName(), error);
        if (!view) return {};
        views.push_back(view);
    }
    return views;
}

std::shared_ptr<const MediumRemap> CreateInstanceMediumRemap(
    const std::vector<std::shared_ptr<InstanceGridMedium>> &prototypeMedia,
    const Transform &prototypeToWorld, const std::string &prototypeName,
    std::string *error) {
    (void)prototypeName;
    if (!ValidateAffineTransform(prototypeToWorld, "instance placement",
                                 error))
        return nullptr;
    std::vector<std::shared_ptr<InstanceGridMedium>> views =
        CreateInstancePlacementViews(prototypeMedia, prototypeToWorld, error);
    if (views.size() != prototypeMedia.size()) return nullptr;
    return CreateInstanceMediumRemap(prototypeMedia, views);
}

std::shared_ptr<const MediumRemap> CreateInstanceMediumRemap(
    const std::vector<std::shared_ptr<InstanceGridMedium>> &prototypeMedia,
    const std::vector<std::shared_ptr<InstanceGridMedium>> &placementViews) {
    CHECK_EQ(prototypeMedia.size(), placementViews.size());
    std::shared_ptr<InstanceGridMediumRemap> remap =
        std::make_shared<InstanceGridMediumRemap>(placementViews);
    for (size_t i = 0; i < prototypeMedia.size(); ++i)
        remap->Add(prototypeMedia[i].get(), placementViews[i].get());
    return remap;
}

// InstanceGridMedium Factory Method Definitions
std::shared_ptr<InstanceGridMedium> CreateInstanceGridMedium(
    const ParamSet &paramSet, const Transform &definitionTransform,
    const std::string &prototypeName, std::string *error) {
    // Match how the heterogeneous medium resolves its scattering
    // coefficients (preset, scale, sigma_a, sigma_s, g).
    Float sig_a_rgb[3] = {.0011f, .0024f, .014f},
          sig_s_rgb[3] = {2.55f, 3.21f, 3.77f};
    Spectrum sig_a = Spectrum::FromRGB(sig_a_rgb),
             sig_s = Spectrum::FromRGB(sig_s_rgb);
    std::string preset = paramSet.FindOneString("preset", "");
    bool found = GetMediumScatteringProperties(preset, &sig_a, &sig_s);
    if (preset != "" && !found)
        Warning("Material preset \"%s\" not found.  Using defaults.",
                preset.c_str());
    Float scale = paramSet.FindOneFloat("scale", 1.f);
    Float g = paramSet.FindOneFloat("g", 0.0f);
    sig_a = paramSet.FindOneSpectrum("sigma_a", sig_a) * scale;
    sig_s = paramSet.FindOneSpectrum("sigma_s", sig_s) * scale;

    int nitems;
    const Float *data = paramSet.FindFloat("density", &nitems);
    if (!data) {
        if (error)
            *error = "no \"density\" values provided for instancegrid medium";
        return nullptr;
    }
    // Guard the parameter/expectation product against overflow.  nx/ny/nz
    // are user-provided, so multiply stepwise with a bound rather than
    // trusting a raw triple product.
    int64_t nx64 = paramSet.FindOneInt("nx", 1);
    int64_t ny64 = paramSet.FindOneInt("ny", 1);
    int64_t nz64 = paramSet.FindOneInt("nz", 1);
    int64_t expected = 1;
    bool overflow = nx64 <= 0 || ny64 <= 0 || nz64 <= 0;
    int64_t dims[3] = {nx64, ny64, nz64};
    for (int i = 0; i < 3 && !overflow; ++i) {
        if (dims[i] > int64_t(std::numeric_limits<int>::max()) ||
            expected > int64_t(std::numeric_limits<int>::max()) / dims[i])
            overflow = true;
        else
            expected *= dims[i];
    }
    if (overflow || expected != int64_t(nitems)) {
        if (error)
            *error = StringPrintf(
                "instancegrid medium has %d density values; expected "
                "nx*ny*nz = %" PRId64 " (nx=%" PRId64 ", ny=%" PRId64
                ", nz=%" PRId64 ")",
                nitems, overflow ? int64_t(-1) : expected, nx64, ny64, nz64);
        return nullptr;
    }
    Point3f p0 = paramSet.FindOnePoint3f("p0", Point3f(0.f, 0.f, 0.f));
    Point3f p1 = paramSet.FindOnePoint3f("p1", Point3f(1.f, 1.f, 1.f));
    if (!std::isfinite(p0.x) || !std::isfinite(p0.y) ||
        !std::isfinite(p0.z) || !std::isfinite(p1.x) ||
        !std::isfinite(p1.y) || !std::isfinite(p1.z) ||
        p1.x == p0.x || p1.y == p0.y || p1.z == p0.z) {
        if (error)
            *error = StringPrintf(
                "instancegrid medium has a degenerate density box "
                "(p0 = %f %f %f, p1 = %f %f %f)",
                p0.x, p0.y, p0.z, p1.x, p1.y, p1.z);
        return nullptr;
    }
    // The definition transform maps the density box's local frame into
    // prototype coordinates; p0/p1 place the normalized [0,1]^3 cube in
    // that local frame exactly as for the heterogeneous medium.
    Transform data2PrototypeLocal =
        Translate(Vector3f(p0)) *
        Scale(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
    Transform densityToPrototype = definitionTransform * data2PrototypeLocal;

    std::shared_ptr<const InstanceGridDensity> grid =
        InstanceGridDensity::Create(sig_a, sig_s, g, int(nx64), int(ny64),
                                    int(nz64), densityToPrototype, data,
                                    error);
    if (!grid) return nullptr;
    paramSet.ReportUnused();
    return InstanceGridMedium::CreatePrototype(std::move(grid), prototypeName);
}

}  // namespace pbrt
