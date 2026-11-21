/*
    material_replay.cpp

    Implementation of the frozen-proposal plastic material record.  See
    material_replay.h for the contract.  All directional decisions are made
    once during Record() with the real PlasticMaterial BSDF code; the record
    then owns plain data so that Evaluate()/Backward() keep working after the
    Scene, the MemoryArena and the proposal materials are gone.
*/

#include "stdafx.h"
#include "integrators/material_replay.h"

#include "scene.h"
#include "intersection.h"
#include "reflection.h"
#include "memory.h"
#include "rng.h"
#include "materials/plastic.h"
#include "textures/constant.h"
#include "lights/point.h"

#include <cmath>
#include <unordered_map>

namespace {

// Local small utilities ---------------------------------------------------

void SetError(std::string *error, const char *message) {
    if (error) *error = message;
}


void SpectrumToRGB(const Spectrum &s, float rgb[3]) {
    s.ToRGB(rgb);
}


Spectrum RGBToSpectrum(const float rgb[3]) {
    return Spectrum::FromRGB(rgb);
}


bool FiniteRGB(const float v[3]) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}


// Scalar Fresnel dielectric reflectance, mirroring
// FresnelDielectric::Evaluate / FrDiel with eta_i = 1.5 and eta_t = 1.
float PlasticFresnel(float cosi) {
    cosi = Clamp(cosi, -1.f, 1.f);
    float ei = 1.5f, et = 1.f;
    if (!(cosi > 0.f)) swap(ei, et);
    float sint = ei / et * sqrtf(max(0.f, 1.f - cosi * cosi));
    if (sint >= 1.f) return 1.f;
    float cost = sqrtf(max(0.f, 1.f - sint * sint));
    float rParl = ((et * cosi) - (ei * cost)) /
                  ((et * cosi) + (ei * cost));
    float rPerp = ((ei * cosi) - (et * cost)) /
                  ((ei * cosi) + (et * cost));
    return 0.5f * (rParl * rParl + rPerp * rPerp);
}


struct PlasticEval {
    float f[3];       // full plastic f (lambertian + microfacet)
    float hSpec;      // specular factor multiplying ks, per direction
    float dHdRough;   // d(hSpec)/d(roughness); ks is pulled out separately
};


// Evaluate the plastic model in BSDF local coordinates for one direction
// pair.  The math is a direct transcription of Lambertian::f,
// Microfacet::f, Blinn::D, Microfacet::G and FresnelDielectric::Evaluate.
// hSpec is the channel-independent multiplier H with f_spec_c = ks_c * H,
// which keeps the derivatives well defined at zero target reflectance.
PlasticEval EvalPlastic(const MaterialReplayParams &p,
                        const Vector &wo, const Vector &wi,
                        bool wantDerivative) {
    PlasticEval out;
    for (int c = 0; c < 3; ++c) out.f[c] = 0.f;
    out.hSpec = 0.f;
    out.dHdRough = 0.f;

    // Lambertian component: f_d = Kd / pi.
    for (int c = 0; c < 3; ++c)
        out.f[c] += p.kd[c] * INV_PI;

    float cosThetaO = fabsf(wo.z);
    float cosThetaI = fabsf(wi.z);
    if (cosThetaI == 0.f || cosThetaO == 0.f) return out;

    Vector wh = wi + wo;
    if (wh.x == 0.f && wh.y == 0.f && wh.z == 0.f) return out;
    wh = Normalize(wh);

    float woDotWh = fabsf(Dot(wo, wh));
    if (woDotWh <= 0.f) return out;
    float cosThetaH = Dot(wi, wh);
    float nDotWh = fabsf(wh.z);

    // Blinn exponent e = 1 / roughness (Blinn() clamps to 10000; the valid
    // roughness range [0.01,1] keeps e in [1,100]).
    float e = 1.f / p.roughness;
    if (e > 10000.f || std::isnan(e)) e = 10000.f;

    float blinnD = (e + 2.f) * INV_TWOPI * powf(nDotWh, e);
    float geoG = min(1.f,
                     min((2.f * nDotWh * cosThetaO / woDotWh),
                         (2.f * nDotWh * cosThetaI / woDotWh)));
    float fresnel = PlasticFresnel(cosThetaH);
    float denom = 4.f * cosThetaI * cosThetaO;

    float h = blinnD * geoG * fresnel / denom;
    out.hSpec = h;
    for (int c = 0; c < 3; ++c)
        out.f[c] += p.ks[c] * h;

    if (wantDerivative) {
        // dD/droughness, D = (e+2)/(2*pi) * c^e and de/drough = -e/rough.
        float dDdRough = 0.f;
        if (nDotWh > 0.f) {
            float dedRough = -1.f / (p.roughness * p.roughness);
            float logC = logf(nDotWh); // c <= 1, and c^e ln c -> 0 as c -> 0
            dDdRough = dedRough * INV_TWOPI * powf(nDotWh, e) *
                       (1.f + (e + 2.f) * logC);
            if (!std::isfinite(dDdRough)) dDdRough = 0.f;
        }
        out.dHdRough = dDdRough * geoG * fresnel / denom;
    }
    return out;
}


// Frozen per-light direct lighting sample at one vertex.
struct FrozenDirect {
    Vector wiLocal;
    float absCos;
    bool sameSide; // geometric-normal hemisphere gate used by BSDF::f
    float lightRGB[3]; // zeroed when the finite shadow ray is occluded
};


// Frozen path vertex: only plain owned data.
struct FrozenVertex {
    int idIndex;
    Vector woLocal;
    // Continuation (invalid when failed or last vertex).
    Vector wiLocal;
    float absCos;
    float proposalPdf;
    bool continues;
    bool failed;
    std::vector<FrozenDirect> direct;
};


struct FrozenPath {
    std::vector<FrozenVertex> verts;
};

} // anonymous namespace


// MaterialReplay::IdProposal / Path are declared in the header as nested
// incomplete types; the owning definitions live here alongside the frozen
// record structures above, so the header stays allocation free.
struct MaterialReplay::IdProposal {
    int id;
    MaterialReplayParams proposal;
};


struct MaterialReplay::Path {
    FrozenPath data;
};


// MaterialReplay lifecycle -------------------------------------------------

MaterialReplay::MaterialReplay() {
}


MaterialReplay::~MaterialReplay() {
    for (size_t i = 0; i < ids.size(); ++i) delete ids[i];
    for (size_t i = 0; i < paths.size(); ++i) delete paths[i];
}


int MaterialReplay::IdAt(int index) const {
    return ids[index]->id;
}


int MaterialReplay::PathNumVertices(int rayIndex) const {
    if (rayIndex < 0 || rayIndex >= int(paths.size())) return -1;
    return int(paths[rayIndex]->data.verts.size());
}


bool MaterialReplay::PathEndedFailed(int rayIndex) const {
    if (rayIndex < 0 || rayIndex >= int(paths.size())) return false;
    const std::vector<FrozenVertex> &verts =
        paths[rayIndex]->data.verts;
    return !verts.empty() && verts.back().failed;
}


MaterialReplay::IdProposal *MaterialReplay::FindId(int id) {
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i]->id == id) return ids[i];
    return NULL;
}


const MaterialReplay::IdProposal *MaterialReplay::FindId(int id) const {
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i]->id == id) return ids[i];
    return NULL;
}


bool MaterialReplay::ValidateEditable(const MaterialReplayParams &p,
                                      std::string *error) {
    if (!FiniteRGB(p.kd) || !FiniteRGB(p.ks) ||
        !std::isfinite(p.roughness)) {
        SetError(error, "material parameters must be finite");
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        if (p.kd[c] < 0.f || p.kd[c] > 1.f) {
            SetError(error, "Kd components must lie in [0,1]");
            return false;
        }
        if (p.ks[c] < 0.f || p.ks[c] > 1.f) {
            SetError(error, "Ks components must lie in [0,1]");
            return false;
        }
    }
    if (p.roughness < 0.01f || p.roughness > 1.f) {
        SetError(error, "roughness must lie in [0.01,1]");
        return false;
    }
    return true;
}


bool MaterialReplay::ValidateProposal(const MaterialReplayParams &p,
                                      std::string *error) {
    if (!ValidateEditable(p, error)) return false;
    for (int c = 0; c < 3; ++c) {
        if (!(p.kd[c] > 0.f)) {
            SetError(error, "proposal Kd components must be strictly positive");
            return false;
        }
        if (!(p.ks[c] > 0.f)) {
            SetError(error, "proposal Ks components must be strictly positive");
            return false;
        }
    }
    return true;
}


bool MaterialReplay::BindProposal(int id, const MaterialReplayParams &proposal,
                                  std::string *error) {
    if (!ValidateProposal(proposal, error)) return false;
    if (FindId(id)) {
        SetError(error, "material id is already bound");
        return false;
    }
    IdProposal *bound = new IdProposal;
    bound->id = id;
    bound->proposal = proposal;
    ids.push_back(bound);
    return true;
}


bool MaterialReplay::BindPrimitive(int id, uint32_t primitiveId,
                                   std::string *error) {
    int idIndex = -1;
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i]->id == id) { idIndex = int(i); break; }
    if (idIndex < 0) {
        SetError(error, "BindPrimitive: material id has no proposal binding");
        return false;
    }
    primitiveToId[primitiveId] = idIndex;
    return true;
}


// Scene feature validation -------------------------------------------------

bool MaterialReplayValidateScene(const Scene *scene, std::string *error) {
    if (!scene) {
        SetError(error, "scene must not be null");
        return false;
    }
    if (!scene->aggregate) {
        SetError(error, "scene must contain geometry");
        return false;
    }
    if (scene->volumeRegion) {
        SetError(error, "participating media are not supported");
        return false;
    }
    for (size_t i = 0; i < scene->lights.size(); ++i) {
        if (!dynamic_cast<const PointLight *>(scene->lights[i])) {
            SetError(error,
                     "only PointLight lights are supported; area and other "
                     "light types are rejected");
            return false;
        }
    }
    return true;
}


// Recording ----------------------------------------------------------------

bool MaterialReplay::Record(const Scene *scene,
                            const std::vector<MaterialReplayRay> &rays,
                            int maxSurfaceVertices,
                            std::string *error) {
    if (!MaterialReplayValidateScene(scene, error)) return false;
    if (ids.empty()) {
        SetError(error, "no material ids have been bound");
        return false;
    }
    if (rays.empty()) {
        SetError(error, "the ray batch must not be empty");
        return false;
    }
    if (maxSurfaceVertices < 1) {
        SetError(error, "maxSurfaceVertices must be at least one");
        return false;
    }

    // Build the proposal PlasticMaterial objects.  Constant textures only,
    // matching the supported feature set; every BSDF decision below is made
    // by the production plastic/BSDF code.
    std::vector<Reference<Material> > proposalMaterials(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const MaterialReplayParams &pp = ids[i]->proposal;
        Reference<Texture<Spectrum> > kdTex =
            new ConstantTexture<Spectrum>(RGBToSpectrum(pp.kd));
        Reference<Texture<Spectrum> > ksTex =
            new ConstantTexture<Spectrum>(RGBToSpectrum(pp.ks));
        Reference<Texture<float> > roughTex =
            new ConstantTexture<float>(pp.roughness);
        proposalMaterials[i] =
            new PlasticMaterial(kdTex, ksTex, roughTex,
                                Reference<Texture<float> >());
    }

    std::unordered_map<uint32_t, int> primToId = primitiveToId;
    std::vector<Path *> newPaths;
    newPaths.reserve(rays.size());
    MemoryArena arena;

    for (size_t r = 0; r < rays.size(); ++r) {
        Vector d = rays[r].direction;
        if (!std::isfinite(d.x) || !std::isfinite(d.y) ||
            !std::isfinite(d.z) || d.LengthSquared() == 0.f ||
            !std::isfinite(rays[r].origin.x) ||
            !std::isfinite(rays[r].origin.y) ||
            !std::isfinite(rays[r].origin.z)) {
            SetError(error, "ray origins and directions must be finite and "
                            "directions non-zero");
            for (size_t i = 0; i < newPaths.size(); ++i) delete newPaths[i];
            return false;
        }
        d = Normalize(d);

        Path *path = new Path;
        newPaths.push_back(path);
        RNG rng(rays[r].seed);

        RayDifferential ray(rays[r].origin, d, 0.f);
        Intersection isect;
        if (!scene->Intersect(ray, &isect)) {
            // Black background: rays that miss everything stay empty.
            continue;
        }

        for (int depth = 0; depth < maxSurfaceVertices; ++depth) {
            std::unordered_map<uint32_t, int>::const_iterator it =
                primToId.find(isect.primitiveId);
            if (it == primToId.end()) {
                SetError(error, "hit a primitive whose material is not bound "
                                "to a plastic id; unsupported materials and "
                                "textures are rejected");
                for (size_t i = 0; i < newPaths.size(); ++i)
                    delete newPaths[i];
                return false;
            }
            if (isect.primitive->GetAreaLight() != NULL) {
                SetError(error, "area lights are not supported");
                for (size_t i = 0; i < newPaths.size(); ++i)
                    delete newPaths[i];
                return false;
            }
            int idIndex = it->second;

            // Obtain the production shading geometry (this runs the shape's
            // GetShadingGeometry and builds the native BSDF frame), then
            // build the proposal BSDF from the bound proposal parameters so
            // that every directional decision uses the frozen proposal.
            BSDF *refBSDF = isect.GetBSDF(ray, arena);

            // Reject anything that is not a constant plastic reflection
            // model: at most one Lambertian and one glossy microfacet term,
            // no specular, transmission or other BxDF components.
            int nComps = refBSDF->NumComponents(BSDF_ALL);
            int nDiff = refBSDF->NumComponents(
                BxDFType(BSDF_REFLECTION | BSDF_DIFFUSE));
            int nGloss = refBSDF->NumComponents(
                BxDFType(BSDF_REFLECTION | BSDF_GLOSSY));
            int nSpec = refBSDF->NumComponents(BSDF_SPECULAR);
            int nTrans = refBSDF->NumComponents(BSDF_TRANSMISSION);
            if (nComps < 1 || nComps > 2 || nSpec != 0 || nTrans != 0 ||
                nDiff + nGloss != nComps || nDiff > 1 || nGloss > 1) {
                SetError(error, "unsupported material on a bound primitive: "
                                "only constant PlasticMaterial is accepted");
                for (size_t i = 0; i < newPaths.size(); ++i)
                    delete newPaths[i];
                return false;
            }

            // Texture and bump-map rejection: a constant plastic evaluates
            // to the same BRDF at every surface point.  Re-evaluate the
            // production material at several perturbed differential
            // geometries (including integer UV shifts that cross every
            // checker-style cell boundary) and compare fixed evaluations.
            // Snapshot the shading geometry first: the arena is reset
            // between probes and would otherwise leave refBSDF dangling.
            DifferentialGeometry dgsSnapshot = refBSDF->dgShading;
            // Probe along the geometric normal: the plastic f is guaranteed
            // nonzero there for any non-black material, independent of the
            // view direction.
            Vector probeWo = Normalize(Vector(isect.dg.nn));
            Spectrum fRef = refBSDF->f(probeWo, probeWo);
            float fRefRGB[3];
            SpectrumToRGB(fRef, fRefRGB);
            const float probeShift[4][3] = {
                {0.137f, 0.219f, 0.07f},
                {1.000f, 0.000f, 0.00f},
                {0.000f, 1.000f, 0.00f},
                {3.700f, 2.300f, 0.13f},
            };
            bool nonConstant = false;
            for (int pi = 0; pi < 4 && !nonConstant; ++pi) {
                DifferentialGeometry dg2 = isect.dg;
                dg2.u += probeShift[pi][0];
                dg2.v += probeShift[pi][1];
                dg2.p = isect.dg.p + isect.dg.dpdu * probeShift[pi][2] +
                         isect.dg.dpdv * probeShift[pi][1];
                BSDF *bsdf2 = isect.primitive->GetBSDF(
                    dg2, isect.ObjectToWorld, arena);
                Spectrum f2 = bsdf2->f(probeWo, probeWo);
                float f2RGB[3];
                SpectrumToRGB(f2, f2RGB);
                for (int c = 0; c < 3; ++c) {
                    float scale = max(fabsf(fRefRGB[c]), fabsf(f2RGB[c]));
                    if (scale > 1e-6f &&
                        fabsf(fRefRGB[c] - f2RGB[c]) > 1e-4f * scale) {
                        nonConstant = true;
                        break;
                    }
                }
                arena.FreeAll();
            }
            if (nonConstant) {
                SetError(error,
                         "textured or bump-mapped material on a bound "
                         "primitive: only constant PlasticMaterial is "
                         "accepted");
                for (size_t i = 0; i < newPaths.size(); ++i)
                    delete newPaths[i];
                return false;
            }

            BSDF *bsdf = proposalMaterials[idIndex]->GetBSDF(
                isect.dg, dgsSnapshot, arena);

            FrozenVertex v;
            v.idIndex = idIndex;
            v.continues = false;
            v.failed = false;
            v.absCos = 0.f;
            v.proposalPdf = 0.f;

            const Point &p = bsdf->dgShading.p;
            const Normal &n = bsdf->dgShading.nn;
            Vector wo = -ray.d;
            v.woLocal = bsdf->WorldToLocal(wo);

            // Direct lighting from every point light, each with its own
            // finite shadow ray.  PointLight::Sample_L ignores its random
            // sample, so no RNG state is consumed here.
            v.direct.reserve(scene->lights.size());
            for (size_t li = 0; li < scene->lights.size(); ++li) {
                const Light *light = scene->lights[li];
                FrozenDirect fd;
                Vector wiWorld;
                float lightPdf = 0.f;
                VisibilityTester vis;
                LightSample lightSample(0.5f, 0.5f, 0.5f);
                Spectrum Li = light->Sample_L(p, isect.rayEpsilon,
                                              lightSample, ray.time,
                                              &wiWorld, &lightPdf, &vis);
                fd.absCos = AbsDot(wiWorld, n);
                // BSDF::f discards reflection terms when wi and wo lie on
                // different sides of the geometric normal; freeze that gate.
                fd.sameSide = Dot(wiWorld, isect.dg.nn) *
                                  Dot(wo, isect.dg.nn) > 0.f;
                fd.wiLocal = bsdf->WorldToLocal(wiWorld);
                SpectrumToRGB(Li, fd.lightRGB);
                // Freeze the finite shadow result: occlusion simply zeroes
                // the incident light and therefore its contribution.
                if (!vis.Unoccluded(scene))
                    for (int c = 0; c < 3; ++c) fd.lightRGB[c] = 0.f;
                v.direct.push_back(fd);
            }

            if (depth + 1 == maxSurfaceVertices) {
                // Surface vertex budget exhausted: no continuation.
                path->data.verts.push_back(v);
                break;
            }

            // Successor direction comes from the proposal BSDF alone.
            BSDFSample outgoing(rng);
            Vector wiWorld;
            float pdf = 0.f;
            BxDFType flags = BSDF_ALL;
            Spectrum f = bsdf->Sample_f(wo, &wiWorld, outgoing, &pdf,
                                        BSDF_ALL, &flags);
            if (f.IsBlack() || pdf == 0.f || !std::isfinite(pdf)) {
                // Failed sample: keep the direct contribution and end the
                // path with zero contribution.  No re-draw is allowed.
                v.failed = true;
                path->data.verts.push_back(v);
                break;
            }

            v.continues = true;
            v.wiLocal = bsdf->WorldToLocal(wiWorld);
            v.absCos = AbsDot(wiWorld, n);
            v.proposalPdf = pdf;
            path->data.verts.push_back(v);

            RayDifferential nextRay(p, wiWorld, ray, isect.rayEpsilon);
            Intersection nextIsect;
            if (!scene->Intersect(nextRay, &nextIsect)) {
                // Black background, no environment light: the path ends.
                break;
            }
            ray = nextRay;
            isect = nextIsect;
            arena.FreeAll();
        }
        arena.FreeAll();
    }

    // Success: replace the previous record atomically.
    for (size_t i = 0; i < paths.size(); ++i) delete paths[i];
    paths.swap(newPaths);
    return true;
}


// Re-evaluation ------------------------------------------------------------

bool MaterialReplay::validateTargets(
        const std::vector<MaterialReplayParams> &targets,
        std::string *error) const {
    if (targets.size() != ids.size()) {
        SetError(error, "target parameter count must match the bound id count");
        return false;
    }
    for (size_t i = 0; i < targets.size(); ++i)
        if (!ValidateEditable(targets[i], error)) return false;
    return true;
}


bool MaterialReplay::Evaluate(
        const std::vector<MaterialReplayParams> &targets,
        std::vector<Spectrum> *radiance,
        std::string *error) const {
    if (!radiance) {
        SetError(error, "radiance output must not be null");
        return false;
    }
    if (!validateTargets(targets, error)) return false;

    radiance->assign(paths.size(), Spectrum(0.f));
    for (size_t r = 0; r < paths.size(); ++r) {
        const std::vector<FrozenVertex> &verts = paths[r]->data.verts;
        float prefix[3] = {1.f, 1.f, 1.f};
        float rgb[3] = {0.f, 0.f, 0.f};

        for (size_t k = 0; k < verts.size(); ++k) {
            const FrozenVertex &v = verts[k];
            const MaterialReplayParams &tp = targets[v.idIndex];

            // Direct lighting at this vertex uses the target f.
            for (size_t li = 0; li < v.direct.size(); ++li) {
                const FrozenDirect &fd = v.direct[li];
                if (!fd.sameSide) continue; // BSDF::f geometric-normal gate
                PlasticEval pe = EvalPlastic(tp, v.woLocal, fd.wiLocal, false);
                for (int c = 0; c < 3; ++c)
                    rgb[c] += prefix[c] * fd.lightRGB[c] * fd.absCos *
                              pe.f[c];
            }

            if (!v.continues) break;

            // Reweight the fixed successor with the target f and the
            // recorded proposal pdf.  The target pdf never appears here.
            PlasticEval pe = EvalPlastic(tp, v.woLocal, v.wiLocal, false);
            for (int c = 0; c < 3; ++c)
                prefix[c] *= pe.f[c] * v.absCos / v.proposalPdf;
        }
        (*radiance)[r] = RGBToSpectrum(rgb);
    }
    return true;
}


// Reverse mode -------------------------------------------------------------

bool MaterialReplay::Backward(
        const std::vector<MaterialReplayParams> &targets,
        const std::vector<Spectrum> &adjoints,
        std::vector<MaterialReplayGradient> *gradients,
        std::string *error) const {
    if (!gradients) {
        SetError(error, "gradient output must not be null");
        return false;
    }
    if (!validateTargets(targets, error)) return false;
    if (adjoints.size() != paths.size()) {
        SetError(error, "one RGB adjoint is required per recorded ray");
        return false;
    }

    std::vector<std::vector<double> > gKd(
        ids.size(), std::vector<double>(3, 0.0));
    std::vector<std::vector<double> > gKs(
        ids.size(), std::vector<double>(3, 0.0));
    std::vector<double> gRough(ids.size(), 0.0);

    for (size_t r = 0; r < paths.size(); ++r) {
        const std::vector<FrozenVertex> &verts = paths[r]->data.verts;
        float adjoint[3];
        SpectrumToRGB(adjoints[r], adjoint);
        for (int c = 0; c < 3; ++c)
            if (!std::isfinite(adjoint[c])) {
                SetError(error, "adjoints must be finite");
                return false;
            }

        // Forward prefix products T_k and direct radiance D_k, then the
        // suffix radiance R_k = D_k + m_k R_{k+1}, all per RGB channel.
        size_t n = verts.size();
        std::vector<std::vector<float> > T(n, std::vector<float>(3, 1.f));
        std::vector<std::vector<float> > D(n, std::vector<float>(3, 0.f));
        std::vector<std::vector<float> > R(n + 1,
                                            std::vector<float>(3, 0.f));

        float prefix[3] = {1.f, 1.f, 1.f};
        for (size_t k = 0; k < n; ++k) {
            const FrozenVertex &v = verts[k];
            const MaterialReplayParams &tp = targets[v.idIndex];
            for (int c = 0; c < 3; ++c) T[k][c] = prefix[c];

            for (size_t li = 0; li < v.direct.size(); ++li) {
                const FrozenDirect &fd = v.direct[li];
                if (!fd.sameSide) continue; // BSDF::f geometric-normal gate
                PlasticEval pe =
                    EvalPlastic(tp, v.woLocal, fd.wiLocal, true);
                for (int c = 0; c < 3; ++c)
                    D[k][c] += fd.lightRGB[c] * fd.absCos * pe.f[c];
            }

            if (v.continues) {
                PlasticEval pe =
                    EvalPlastic(tp, v.woLocal, v.wiLocal, true);
                for (int c = 0; c < 3; ++c)
                    prefix[c] *= pe.f[c] * v.absCos / v.proposalPdf;
            }
        }

        for (size_t kk = n; kk > 0; --kk) {
            size_t k = kk - 1;
            const FrozenVertex &v = verts[k];
            for (int c = 0; c < 3; ++c) R[k][c] = D[k][c];
            if (v.continues) {
                const MaterialReplayParams &tp = targets[v.idIndex];
                PlasticEval pe =
                    EvalPlastic(tp, v.woLocal, v.wiLocal, true);
                float m[3];
                for (int c = 0; c < 3; ++c) {
                    m[c] = pe.f[c] * v.absCos / v.proposalPdf;
                    R[k][c] += m[c] * R[k + 1][c];
                }
            }
        }

        // Distribute adjoints onto the local f evaluations, then chain into
        // the seven shared parameters.  Every occurrence of an id adds in.
        for (size_t k = 0; k < n; ++k) {
            const FrozenVertex &v = verts[k];
            const MaterialReplayParams &tp = targets[v.idIndex];
            int id = v.idIndex;

            // Direct lights: adj_f_c = g_c T_k_c A_l_c.
            for (size_t li = 0; li < v.direct.size(); ++li) {
                const FrozenDirect &fd = v.direct[li];
                if (!fd.sameSide) continue; // BSDF::f geometric-normal gate
                PlasticEval pe =
                    EvalPlastic(tp, v.woLocal, fd.wiLocal, true);
                for (int c = 0; c < 3; ++c) {
                    float adjF = adjoint[c] * T[k][c] *
                                 fd.lightRGB[c] * fd.absCos;
                    if (adjF == 0.f) continue;
                    gKd[id][c] += double(adjF) * INV_PI;
                    gKs[id][c] += double(adjF) * pe.hSpec;
                    gRough[id] += double(adjF) * tp.ks[c] * pe.dHdRough;
                }
            }

            // Continuation: adj_f_c = g_c T_k_c R_{k+1,c} |cos|/pdf.
            if (v.continues) {
                PlasticEval pe =
                    EvalPlastic(tp, v.woLocal, v.wiLocal, true);
                for (int c = 0; c < 3; ++c) {
                    float adjF = adjoint[c] * T[k][c] * R[k + 1][c] *
                                 v.absCos / v.proposalPdf;
                    if (adjF == 0.f) continue;
                    gKd[id][c] += double(adjF) * INV_PI;
                    gKs[id][c] += double(adjF) * pe.hSpec;
                    gRough[id] += double(adjF) * tp.ks[c] * pe.dHdRough;
                }
            }
        }
    }

    gradients->resize(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        MaterialReplayGradient &g = (*gradients)[i];
        for (int c = 0; c < 3; ++c) {
            g.dKd[c] = float(gKd[i][c]);
            g.dKs[c] = float(gKs[i][c]);
        }
        g.dRoughness = float(gRough[i]);
    }
    return true;
}
