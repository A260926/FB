
/*
    Specular path replay with analytic adjoints for pbrt-v2 scenes.

    This header declares a self-contained mirror/matte path replay interface.
    Forward tracing reuses the existing Scene::Intersect / Scene::IntersectP
    entry points (and therefore the existing BVH, TriangleMesh refinement and
    TransformedPrimitive nesting); no second intersection engine is built.
    The returned ReplayRecord owns every value needed to differentiate the
    traced paths, so it outlives the Scene, shapes, materials and transforms
    used to create it.  The reverse pass is a hand-written adjoint: it never
    perturbs parameters, never re-traces the scene and uses no automatic
    differentiation library.

    Supported scene subset (everything else is rejected explicitly):
      - TriangleMesh shapes only, with no alpha texture,
      - GeometricPrimitive leaves and TransformedPrimitive instances
        (arbitrarily nested, static, finite invertible affine transforms),
      - MirrorMaterial with a constant Kr texture and no bump map,
      - MatteMaterial with a constant Kd texture, sigma == 0 and no bump map,
      - PointLight lights only, black background.

    Differentiated parameters:
      - source TriangleMesh constructor P arrays, identified by a stable
        parameter ID bound with BindMesh(),
      - each TransformedPrimitive's local-to-parent 3x4 affine matrix in
        row-major order, identified by a stable parameter ID bound with
        BindInstance().
    A mesh or instance without a binding still participates in forward
    rendering but receives no gradient slot.  The fixed ObjectToWorld of a
    source shape is constant and never differentiated.
*/

#if defined(_MSC_VER)
#pragma once
#endif

#ifndef PBRT_INTEGRATORS_REPLAY_H
#define PBRT_INTEGRATORS_REPLAY_H

#include "pbrt.h"
#include "geometry.h"

class Scene;
class TriangleMesh;
class TransformedPrimitive;

// Replay declarations
struct ReplayRay {
    ReplayRay() { }
    ReplayRay(const Point &org, const Vector &dir) : o(org), d(dir) { }
    Point o;
    Vector d;
};

struct ReplayColor {
    ReplayColor() { rgb[0] = rgb[1] = rgb[2] = 0.f; }
    float rgb[3];
};

#define REPLAY_PARAM_POSITION 0
#define REPLAY_PARAM_MATRIX   1

// One gradient block per bound parameter ID.  Position blocks hold xyz
// triples per vertex of the source mesh P array; matrix blocks hold the 12
// row-major entries of the instance's local-to-parent 3x4 affine.  Multiple
// meshes or instances sharing one ID accumulate into a single block, whose
// count is the union of their entries (all contributors must agree on the
// block layout: same vertex count for meshes, 12 for instances).
struct ReplayGradientBlock {
    int parameterId;
    int kind;             // REPLAY_PARAM_POSITION / REPLAY_PARAM_MATRIX
    int count;            // number of floats
    float *values;        // owned by ReplayGradient
};

struct ReplayGradient {
    int numBlocks;
    ReplayGradientBlock *blocks;
    // Returns the block for a bound ID, or NULL if the ID was never bound.
    ReplayGradientBlock *Find(int parameterId);
    const ReplayGradientBlock *Find(int parameterId) const;
};

struct ReplayRecord;

// Bind stable parameter IDs before running ReplayTrace().  The same ID may
// be bound to several meshes/instances; their contributions accumulate into
// one gradient block.  Pointers must remain valid only until ReplayTrace()
// returns; the record keeps no reference to them.
void ReplayBindMesh(const TriangleMesh *mesh, int parameterId);
void ReplayBindInstance(const TransformedPrimitive *instance, int parameterId);
void ReplayClearBindings();

// Replays every ray through the scene with at most maxBounces mirror
// reflections.  Matte finishes terminate the path with per-point-light
// Lambert direct lighting; mirror finishes continue along the ideal
// reflection direction and multiply by the constant Kr.  Exceeding the
// bounce budget or missing all geometry returns black.  Returns false (and
// fills errorText) when the scene uses an unsupported feature or a ray is
// not finite and non-zero.  The record owns its data and stays valid after
// the scene and all its objects are destroyed.
bool ReplayTrace(const Scene *scene, const vector<ReplayRay> &rays,
                 int maxBounces, ReplayRecord **record,
                 string *errorText = NULL);

// Number of rays stored in the record.
int ReplayNumRays(const ReplayRecord *record);

// Linear RGB colors produced by the forward pass, one entry per input ray.
const ReplayColor *ReplayColors(const ReplayRecord *record);

// Runs the adjoint.  adjoint must hold one RGB seed per ray; the objective
// differentiated is sum_r (g_r . RGB_r).  Each call starts from zero, so
// repeated calls with different seeds are independent.  The gradient blocks
// are indexed by the parameter IDs given at bind time; blocks with no
// bound geometry stay zero-sized.  The returned object is owned by the
// caller and must be released with ReplayFreeGradient().
ReplayGradient *ReplayBackward(const ReplayRecord *record,
                                const vector<ReplayColor> &adjoint);
void ReplayFreeGradient(ReplayGradient *gradient);

// Record lifetime management.
void ReplayReleaseRecord(ReplayRecord *record);

#endif // PBRT_INTEGRATORS_REPLAY_H
