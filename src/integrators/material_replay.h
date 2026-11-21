/*
    material_replay.h

    Frozen-proposal material fitting for the pbrt-v2 CPU path tracer.

    The same batch of world-space rays is traced once through a static
    triangle-mesh scene while the proposal PlasticMaterial BSDFs drive every
    directional decision.  Everything needed to re-evaluate the paths under
    different *target* plastic parameters is copied into a MaterialReplay
    record that owns its data: it stays valid after the Scene, MemoryArena and
    the proposal materials have been destroyed.

    Supported scene features (everything else is explicitly rejected):
      - static TriangleMesh geometry inside the native BVH,
      - constant PlasticMaterial only (no textures, no bump maps),
      - PointLight lights only, with the engine's finite shadow rays,
      - a black background and no participating media (Scene::volumeRegion
        must be NULL).

    The path semantics mirror PathIntegrator::Li with Russian roulette turned
    off: every hit vertex accumulates direct lighting from *all* point lights,
    then the path continues while the surface-vertex budget allows.  Successor
    directions, component selection, survival, occlusion and the number of
    samples are fixed by the proposal BSDF's Sample_f; editing target
    parameters only reweights the fixed paths, never re-samples them.
*/

#ifndef PBRT_INTEGRATORS_MATERIAL_REPLAY_H
#define PBRT_INTEGRATORS_MATERIAL_REPLAY_H

#include "pbrt.h"
#include "geometry.h"
#include "spectrum.h"
#include "rng.h"

#include <string>
#include <unordered_map>
#include <vector>

// MaterialReplay Declarations
struct MaterialReplayParams {
    // Seven editable parameters per shared material id.
    float kd[3]; // RGB diffuse reflectance, each component in [0,1]
    float ks[3]; // RGB specular reflectance, each component in [0,1]
    float roughness; // Blinn 1/roughness exponent driver, in [0.01,1]

    MaterialReplayParams() {
        kd[0] = kd[1] = kd[2] = 0.25f;
        ks[0] = ks[1] = ks[2] = 0.25f;
        roughness = 0.1f;
    }
};


struct MaterialReplayRay {
    // Fixed world-space camera ray.
    Point origin;
    Vector direction;
    uint32_t seed; // RNG seed; draws are reproducible per ray
    MaterialReplayRay() : origin(0.f, 0.f, 0.f), direction(0.f, 0.f, 1.f),
                          seed(1u) { }
    MaterialReplayRay(const Point &o, const Vector &d, uint32_t s)
        : origin(o), direction(d), seed(s) { }
};


struct MaterialReplayGradient {
    // d/du of sum over rays of dot(adjoint, RGB) for one shared material id.
    float dKd[3];
    float dKs[3];
    float dRoughness;
    MaterialReplayGradient() {
        for (int i = 0; i < 3; ++i) { dKd[i] = 0.f; dKs[i] = 0.f; }
        dRoughness = 0.f;
    }
};


class MaterialReplay {
public:
    // MaterialReplay Public Methods
    MaterialReplay();
    ~MaterialReplay();

    // Bind a stable material id to its proposal parameters.  The same id may
    // be attached to any number of scene primitives; every occurrence shares
    // the seven parameters.  Proposal Kd/Ks components must be strictly
    // positive and all values must be finite and inside their ranges.
    bool BindProposal(int id, const MaterialReplayParams &proposal,
                      std::string *error = NULL);

    // Associate a pbrt Primitive::primitiveId with a bound material id.
    // Every surface hit during Record() must resolve to a bound primitive;
    // unbound primitives (and therefore any material not explicitly declared
    // as constant plastic) are rejected.  Multiple primitives may share one
    // id, and the same primitive may be registered more than once.
    bool BindPrimitive(int id, uint32_t primitiveId,
                       std::string *error = NULL);

    // Number and order of the bound material ids; target parameter vectors
    // and gradient vectors follow this same order.
    int NumIds() const { return int(ids.size()); }
    int IdAt(int index) const;

    // Record one batch of fixed rays against the given scene.  Uses
    // Scene::Intersect, the native BVH, the BSDF local coordinate system and
    // the point lights' finite shadow tests.  On failure the record is left
    // untouched and false is returned with an explanation in *error.
    bool Record(const Scene *scene,
                const std::vector<MaterialReplayRay> &rays,
                int maxSurfaceVertices,
                std::string *error = NULL);

    // Number of rays held by the record (zero until a successful Record()).
    int NumRays() const { return int(paths.size()); }

    // Number of surface vertices stored for one recorded ray.  Inspection
    // helper for tests; zero means the ray missed all geometry.
    int PathNumVertices(int rayIndex) const;

    // Whether the ray's last vertex ended on a failed proposal BSDF sample
    // (zero contribution ending, no re-draw).
    bool PathEndedFailed(int rayIndex) const;

    // Re-evaluate the frozen paths under the target parameters.  Returns one
    // RGB radiance value per ray.  The geometry, directions, proposal PDFs
    // and visibility are the recorded ones; only the plastic f and the Blinn
    // normalization change.  Invalid parameters reject atomically and leave
    // the record unchanged.
    bool Evaluate(const std::vector<MaterialReplayParams> &targets,
                  std::vector<Spectrum> *radiance,
                  std::string *error = NULL) const;

    // Reverse mode.  adjoints[i] is the per-ray RGB adjoint; the result for
    // shared id k is the gradient of
    //     sum_rays g . RGB_r(targets)
    // against k's seven parameters.  Repeated occurrences of one id along a
    // path, and across rays, accumulate fully.  Starts from zero on every
    // call and never perturbs parameters or re-traces rays.
    bool Backward(const std::vector<MaterialReplayParams> &targets,
                  const std::vector<Spectrum> &adjoints,
                  std::vector<MaterialReplayGradient> *gradients,
                  std::string *error = NULL) const;

    // Parameter range accessors (shared by the tests and the example).
    static bool ValidateEditable(const MaterialReplayParams &p,
                                 std::string *error);
    static bool ValidateProposal(const MaterialReplayParams &p,
                                 std::string *error);

private:
    // MaterialReplay Private Data
    struct IdProposal;
    struct Path;
    std::vector<IdProposal *> ids;
    std::vector<Path *> paths;
    std::unordered_map<uint32_t, int> primitiveToId;

    // MaterialReplay Private Methods
    const IdProposal *FindId(int id) const;
    IdProposal *FindId(int id);
    bool validateTargets(const std::vector<MaterialReplayParams> &targets,
                         std::string *error) const;
};


// Validate the scene features this module supports.  Exposed so callers can
// reject a configuration before constructing the record.
bool MaterialReplayValidateScene(const Scene *scene, std::string *error);

#endif // PBRT_INTEGRATORS_MATERIAL_REPLAY_H
