
/*
    Specular path replay with analytic adjoints.

    Forward tracing goes through Scene::Intersect / Scene::IntersectP, so the
    existing BVH, TriangleMesh refinement and nested TransformedPrimitive
    instances are reused unchanged.  After each hit the replay stores every
    value the adjoint needs (vertices, barycentrics, normals, transforms,
    light terms, ...) inside an opaque ReplayRecord that owns its memory.
    The reverse pass replays the recorded math as a hand-written adjoint; it
    never touches the scene again and never perturbs parameters.
*/

#include "stdafx.h"
#include "integrators/replay.h"
#include "scene.h"
#include "intersection.h"
#include "primitive.h"
#include "shapes/trianglemesh.h"
#include "accelerators/bvh.h"
#include "materials/mirror.h"
#include "materials/matte.h"
#include "textures/constant.h"
#include "lights/point.h"
#include "transform.h"
#include <map>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// Small linear algebra helpers (3-vectors / 3x4 affine matrices).
// ---------------------------------------------------------------------------
namespace {

struct V3 {
    float x, y, z;
    V3() : x(0.f), y(0.f), z(0.f) { }
    V3(float xx, float yy, float zz) : x(xx), y(yy), z(zz) { }
    explicit V3(const Point &p) : x(p.x), y(p.y), z(p.z) { }
    explicit V3(const Vector &v) : x(v.x), y(v.y), z(v.z) { }
    explicit V3(const Normal &n) : x(n.x), y(n.y), z(n.z) { }
    Point AsPoint() const { return Point(x, y, z); }
    Vector AsVector() const { return Vector(x, y, z); }
    float operator[](int i) const { return (&x)[i]; }
    float &operator[](int i) { return (&x)[i]; }
};

inline V3 operator+(const V3 &a, const V3 &b) {
    return V3(a.x + b.x, a.y + b.y, a.z + b.z);
}
inline V3 operator-(const V3 &a, const V3 &b) {
    return V3(a.x - b.x, a.y - b.y, a.z - b.z);
}
inline V3 operator*(const V3 &a, float s) {
    return V3(a.x * s, a.y * s, a.z * s);
}
inline float Dot(const V3 &a, const V3 &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 Cross(const V3 &a, const V3 &b) {
    return V3(a.y * b.z - a.z * b.y,
              a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}
inline float LengthSquared(const V3 &a) { return Dot(a, a); }

// Row-major 3x4 affine: x' = A x + t, with A the first 3 columns.
struct Affine {
    float m[12];
    Affine() { for (int i = 0; i < 12; ++i) m[i] = 0.f; }
    V3 ApplyPoint(const V3 &p) const {
        return V3(m[0] * p.x + m[1] * p.y + m[2]  * p.z + m[3],
                  m[4] * p.x + m[5] * p.y + m[6]  * p.z + m[7],
                  m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
    }
    V3 ApplyVector(const V3 &v) const {
        return V3(m[0] * v.x + m[1] * v.y + m[2]  * v.z,
                  m[4] * v.x + m[5] * v.y + m[6]  * v.z,
                  m[8] * v.x + m[9] * v.y + m[10] * v.z);
    }
    float Determinant3() const {
        return m[0] * (m[5] * m[10] - m[6] * m[9])
             - m[1] * (m[4] * m[10] - m[6] * m[8])
             + m[2] * (m[4] * m[9]  - m[5] * m[8]);
    }
};

inline Affine AffineFromTransform(const Transform &t) {
    Affine a;
    const Matrix4x4 &M = t.GetMatrix();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            a.m[4 * i + j] = M.m[i][j];
    return a;
}

inline bool FiniteAffine(const Affine &a) {
    for (int i = 0; i < 12; ++i)
        if (isnan(a.m[i]) || isinf(a.m[i])) return false;
    float det = a.Determinant3();
    if (isnan(det) || isinf(det)) return false;
    // Reject effectively singular matrices; a determinant this small cannot
    // support a stable inverse in single precision.
    return fabsf(det) >= 1e-10f;
}

inline bool FiniteV3(const V3 &v) {
    return !isnan(v.x) && !isinf(v.x) &&
           !isnan(v.y) && !isinf(v.y) &&
           !isnan(v.z) && !isinf(v.z);
}

// One recorded instance level (outermost first).
struct InstanceLevel {
    int paramId;             // -1 if unbound
    Affine localToParent;    // differentiated when bound
};

// One recorded surface interaction.
struct HitRecord {
    // Geometry binding.
    int meshParamId;         // -1 if unbound
    int triIndex;
    int vertIndex[3];        // source mesh vertex indices
    Affine meshO2W;          // fixed, non-differentiated source shape transform
    V3 sourceP[3];           // source P values of the three corners
    V3 rayOrigin;            // world-space ray that produced this hit
    V3 rayDir;               // normalized world-space ray direction
    V3 rayDirRaw;            // direction before the entering normalize()
    bool reverseOrientation;
    vector<InstanceLevel> chain;

    // Primal intersection values (world-space Moller-Trumbore graph).
    V3 worldV[3];            // levelPoints[0]
    V3 b1b2;                 // barycentric coordinates
    float tHit;
    // levelPoints[l] holds the three corners in the output space of chain
    // level l-1: [0] is world space, [chain.size()] is mesh-owner space.
    vector<vector<V3> > levelPoints;

    // Material.
    bool isMirror;
    float rho[3];            // Kr or Kd linear RGB

    // Terminal matte lighting (one entry per scene point light).
    struct LightTerm {
        V3 lightPos;
        float intensity[3];
        bool visible;
        V3 delta;            // lightPos - p
        V3 toLight;          // normalized delta
        float dist2;
        float cosine;        // max(0, N . L)
        float rayEpsilon;
    };
    vector<LightTerm> lights;

    // Filled during finalization.
    V3 p;                    // world hit point
    V3 faceNormal;           // normalized winding face normal
    V3 geomNormal;           // dg.nn, including orientation/handedness flip
    float normalSign;        // (RO ^ swapsHandedness) ? -1 : 1
    V3 reflectDir;           // normalized ideal reflection direction
    float rayEpsilon;        // pbrt's recorded ray epsilon (fixed in adjoint)
    float pbrtTHit;          // t stored on the intersecting ray
};

struct RayRecord {
    V3 origin;
    V3 dir;                  // normalized
    ReplayColor color;
    vector<HitRecord> hits;  // empty => missed / black
    bool black;              // miss or bounce budget exceeded
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Opaque record definition.
// ---------------------------------------------------------------------------
struct ReplayRecord {
    vector<RayRecord> rays;
    vector<ReplayColor> colors;
    struct BlockInfo {
        int paramId;
        int kind;
        int count;
    };
    vector<BlockInfo> blocks;
};

// ---------------------------------------------------------------------------
// Bindings (plain process-global maps; pointers only needed until tracing).
// ---------------------------------------------------------------------------
static map<const TriangleMesh *, int> &MeshBindings() {
    static map<const TriangleMesh *, int> m;
    return m;
}
static map<const TransformedPrimitive *, int> &InstanceBindings() {
    static map<const TransformedPrimitive *, int> m;
    return m;
}

void ReplayBindMesh(const TriangleMesh *mesh, int parameterId) {
    MeshBindings()[mesh] = parameterId;
}

void ReplayBindInstance(const TransformedPrimitive *instance, int parameterId) {
    InstanceBindings()[instance] = parameterId;
}

void ReplayClearBindings() {
    MeshBindings().clear();
    InstanceBindings().clear();
}

// ---------------------------------------------------------------------------
// Scene validation and material/texture extraction.
// ---------------------------------------------------------------------------
namespace {

bool ExtractConstantSpectrum(const Texture<Spectrum> *tex, float rgb[3],
                             string *err) {
    if (!tex) { *err = "missing reflectance texture"; return false; }
    if (!tex->ReplayIsConstant()) {
        *err = "only constant reflectance textures are supported";
        return false;
    }
    const ConstantTexture<Spectrum> *ct =
        static_cast<const ConstantTexture<Spectrum> *>(tex);
    Spectrum s = ct->ReplayValue();
    s.ToRGB(rgb);
    for (int i = 0; i < 3; ++i)
        if (isnan(rgb[i]) || isinf(rgb[i])) {
            *err = "non-finite reflectance value";
            return false;
        }
    return true;
}

bool ValidateMaterial(const Material *material, bool *isMirror, float rho[3],
                      string *err) {
    if (!material) { *err = "primitive without material"; return false; }
    if (material->ReplayIsMirrorMaterial()) {
        const MirrorMaterial *mm =
            static_cast<const MirrorMaterial *>(material);
        if (mm->ReplayBumpTexture() != NULL) {
            *err = "bump-mapped mirror material is not supported";
            return false;
        }
        if (!ExtractConstantSpectrum(mm->ReplayKrTexture(), rho, err))
            return false;
        *isMirror = true;
        return true;
    }
    if (material->ReplayIsMatteMaterial()) {
        const MatteMaterial *mm =
            static_cast<const MatteMaterial *>(material);
        if (mm->ReplayBumpTexture() != NULL) {
            *err = "bump-mapped matte material is not supported";
            return false;
        }
        const Texture<float> *sig = mm->ReplaySigmaTexture();
        if (!sig || !sig->ReplayIsConstant()) {
            *err = "only constant sigma=0 matte material is supported";
            return false;
        }
        if (static_cast<const ConstantTexture<float> *>(sig)->ReplayValue()
            != 0.f) {
            *err = "only sigma=0 matte material is supported";
            return false;
        }
        if (!ExtractConstantSpectrum(mm->ReplayKdTexture(), rho, err))
            return false;
        *isMirror = false;
        return true;
    }
    *err = "unsupported material: only constant mirror and sigma=0 matte "
           "are allowed";
    return false;
}

// One way to reach a leaf: the instance chain plus the composed
// WorldToObject pbrt stores on the Intersection for that chain.
struct ResolverEntry {
    Transform worldToObjectKey;
    vector<InstanceLevel> chain;
};

// Full validated topology of the scene.  A shared prototype leaf appears
// once per enclosing instance chain.
struct SceneTopology {
    map<const GeometricPrimitive *, vector<ResolverEntry> > entries;
};

// Matrix comparison for resolver keys (exact match first, tolerance fallback).
bool SameTransform(const Transform &a, const Transform &b) {
    const Matrix4x4 &ma = a.GetMatrix();
    const Matrix4x4 &mb = b.GetMatrix();
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            if (ma.m[i][j] != mb.m[i][j]) {
                float diff = fabsf(ma.m[i][j] - mb.m[i][j]);
                float scale = max(1.f, fabsf(ma.m[i][j]) + fabsf(mb.m[i][j]));
                if (diff > 1e-5f * scale) return false;
            }
        }
    return true;
}

// Recursive topology walk.  w2oAcc is the composed world-to-current-space
// transform of the levels visited so far (outer first).
bool WalkTopology(const Primitive *prim, const Transform &w2oAcc,
                  vector<InstanceLevel> &chain, SceneTopology *topo,
                  string *err) {
    if (prim->ReplayIsTransformedPrimitive()) {
        const TransformedPrimitive *tp =
            static_cast<const TransformedPrimitive *>(prim);
        const AnimatedTransform &at = tp->ReplayWorldToPrimitive();
        if (at.ReplayActuallyAnimated()) {
            *err = "animated (motion) transforms are not supported";
            return false;
        }
        Transform w2p = *at.ReplayStartTransform();
        Affine l2p = AffineFromTransform(Inverse(w2p));
        if (!FiniteAffine(l2p)) {
            *err = "instance transform is not a finite invertible affine";
            return false;
        }
        InstanceLevel level;
        level.paramId = -1;
        map<const TransformedPrimitive *, int>::const_iterator it =
            InstanceBindings().find(tp);
        if (it != InstanceBindings().end()) level.paramId = it->second;
        level.localToParent = l2p;
        chain.push_back(level);
        Transform nextW2O = w2p * w2oAcc;
        if (!WalkTopology(tp->ReplayChildPrimitive(), nextW2O, chain,
                          topo, err))
            return false;
        chain.pop_back();
        return true;
    }
    // An aggregate (e.g. the BVH holding a refined shared prototype) does not
    // transform geometry; descend into its leaf primitives.
    if (prim->ReplayIsBVHAccel()) {
        const BVHAccel *bvh = static_cast<const BVHAccel *>(prim);
        const vector<Reference<Primitive> > &children =
            bvh->ReplayPrimitives();
        for (uint32_t i = 0; i < children.size(); ++i)
            if (!WalkTopology(children[i].GetPtr(), w2oAcc, chain,
                              topo, err))
                return false;
        return true;
    }
    if (!prim->ReplayIsGeometricPrimitive()) {
        *err = "unsupported primitive: only GeometricPrimitive leaves and "
               "TransformedPrimitive instances are allowed";
        return false;
    }
    const GeometricPrimitive *gp =
        static_cast<const GeometricPrimitive *>(prim);
    if (gp->ReplayAreaLight() != NULL) {
        *err = "area lights are not supported";
        return false;
    }
    const Shape *shape = gp->ReplayShape();
    if (!shape->ReplayIsTriangle()) {
        *err = "only Triangle shapes (from TriangleMesh refinement) are "
               "supported";
        return false;
    }
    const Triangle *tri = static_cast<const Triangle *>(shape);
    const TriangleMesh *mesh = tri->ReplayMesh();
    if (mesh->ReplayAlphaTexture() != NULL) {
        *err = "alpha-masked triangle meshes are not supported";
        return false;
    }
    if (mesh->ReplayHasVertexNormals() ||
        mesh->ReplayHasVertexTangents()) {
        *err = "triangle meshes with explicit normals or tangents are not "
               "supported";
        return false;
    }
    bool isMirror;
    float rho[3];
    if (!ValidateMaterial(gp->ReplayMaterial(), &isMirror, rho, err))
        return false;
    // isect.WorldToObject = shapeW2O * (level w2p products); the walk visits
    // outer levels first, matching TransformedPrimitive::Intersect's
    // post-order multiplications.
    ResolverEntry entry;
    entry.worldToObjectKey = w2oAcc;  // accumulated chain only
    entry.chain = chain;
    topo->entries[gp].push_back(entry);
    return true;
}

bool BuildTopology(const Scene *scene, SceneTopology *topo, string *err) {
    if (!scene || !scene->aggregate) {
        *err = "null scene or aggregate";
        return false;
    }
    if (!scene->aggregate->ReplayIsBVHAccel()) {
        *err = "scene aggregate must be a BVHAccel";
        return false;
    }
    if (scene->volumeRegion != NULL) {
        *err = "volume regions are not supported";
        return false;
    }
    for (uint32_t i = 0; i < scene->lights.size(); ++i) {
        if (!scene->lights[i]->ReplayIsPointLight()) {
            *err = "only PointLight light sources are supported";
            return false;
        }
    }
    const BVHAccel *bvh = static_cast<const BVHAccel *>(scene->aggregate);
    const vector<Reference<Primitive> > &prims = bvh->ReplayPrimitives();
    if (prims.empty()) {
        *err = "scene has no intersectable primitives";
        return false;
    }
    Transform identity;
    for (uint32_t i = 0; i < prims.size(); ++i) {
        vector<InstanceLevel> chain;
        if (!WalkTopology(prims[i].GetPtr(), identity, chain, topo, err))
            return false;
    }
    return true;
}

// Resolves a validated hit to the instance chain pbrt actually traversed.
const vector<InstanceLevel> *ResolveChain(const SceneTopology &topo,
        const GeometricPrimitive *gp, const Transform &isectWorldToObject,
        const Transform &shapeWorldToObject, string *err) {
    map<const GeometricPrimitive *,
        vector<ResolverEntry> >::const_iterator it = topo.entries.find(gp);
    if (it == topo.entries.end()) {
        *err = "internal error: hit primitive missing from topology";
        return NULL;
    }
    // The intersection key includes the fixed shape WorldToObject as well.
    Transform fullKey = isectWorldToObject;
    for (uint32_t k = 0; k < it->second.size(); ++k) {
        const ResolverEntry &e = it->second[k];
        Transform expected = shapeWorldToObject * e.worldToObjectKey;
        if (SameTransform(expected, fullKey))
            return &e.chain;
    }
    // Fallback: single-chain leaf.
    if (it->second.size() == 1)
        return &it->second[0].chain;
    *err = "internal error: could not resolve instance chain for hit";
    return NULL;
}

// ---------------------------------------------------------------------------
// Hit construction and geometric finalization.
// ---------------------------------------------------------------------------

// World-space Moller-Trumbore values, mathematically identical to the
// mesh-space solve in Triangle::Intersect (barycentrics are affine
// invariant); this is the graph replayed by the adjoint.
bool SolveTriangle(const V3 &o, const V3 &d, const V3 v[3],
                   float *b1Out, float *b2Out, float *tOut) {
    V3 e1 = v[1] - v[0];
    V3 e2 = v[2] - v[0];
    V3 s1 = Cross(d, e2);
    float divisor = Dot(s1, e1);
    if (divisor == 0.f) return false;
    float invDivisor = 1.f / divisor;
    V3 s = o - v[0];
    float b1 = Dot(s, s1) * invDivisor;
    V3 s2 = Cross(s, e1);
    float b2 = Dot(d, s2) * invDivisor;
    float t = Dot(e2, s2) * invDivisor;
    *b1Out = b1;
    *b2Out = b2;
    *tOut = t;
    return true;
}

void FinalizeHit(HitRecord *h) {
    float b1 = h->b1b2.x, b2 = h->b1b2.y;
    float b0 = 1.f - b1 - b2;
    h->p = h->worldV[0] * b0 + h->worldV[1] * b1 + h->worldV[2] * b2;

    V3 face = Cross(h->worldV[1] - h->worldV[0],
                    h->worldV[2] - h->worldV[0]);
    h->faceNormal = face * (1.f / sqrtf(LengthSquared(face)));
    int neg = 0;
    for (uint32_t i = 0; i < h->chain.size(); ++i)
        if (h->chain[i].localToParent.Determinant3() < 0.f) ++neg;
    bool swaps = (neg & 1) != 0;
    h->normalSign = (h->reverseOrientation ^ swaps) ? -1.f : 1.f;
    h->geomNormal = h->faceNormal * h->normalSign;

    // Ideal reflection about the geometric normal with a unit incident ray.
    V3 d = h->rayDir;
    float alpha = Dot(d, h->geomNormal);
    V3 ru = d - h->geomNormal * (2.f * alpha);
    h->reflectDir = ru * (1.f / sqrtf(LengthSquared(ru)));
}

bool BuildHitRecord(const SceneTopology &topo,
                    const Intersection &isect,
                    const Ray &worldRay, float pbrtTHit, HitRecord *h,
                    string *err) {
    if (!isect.primitive ||
        !isect.primitive->ReplayIsGeometricPrimitive()) {
        *err = "internal error: hit is not a geometric primitive";
        return false;
    }
    const GeometricPrimitive *gp =
        static_cast<const GeometricPrimitive *>(isect.primitive);
    const Triangle *tri = static_cast<const Triangle *>(gp->ReplayShape());
    const TriangleMesh *mesh = tri->ReplayMesh();

    const vector<InstanceLevel> *chain =
        ResolveChain(topo, gp, isect.WorldToObject,
                     *mesh->WorldToObject, err);
    if (!chain) return false;
    h->chain = *chain;

    h->meshParamId = -1;
    map<const TriangleMesh *, int>::const_iterator mit =
        MeshBindings().find(mesh);
    if (mit != MeshBindings().end()) h->meshParamId = mit->second;

    h->triIndex = tri->ReplayTriangleNumber();
    const int *vi = mesh->ReplayVertexIndices();
    const Point *meshPts = mesh->ReplayWorldPoints();
    h->meshO2W = AffineFromTransform(*mesh->ObjectToWorld);
    for (int k = 0; k < 3; ++k) {
        h->vertIndex[k] = vi[3 * h->triIndex + k];
        // mesh->p holds ObjectToWorld(source P); invert with the fixed shape
        // transform to recover the source P array entry.
        h->sourceP[k] = V3((*mesh->WorldToObject)(meshPts[h->vertIndex[k]]));
    }

    h->rayOrigin = V3(worldRay.o);
    float dlen = sqrtf(LengthSquared(V3(worldRay.d)));
    h->rayDirRaw = V3(worldRay.d);
    h->rayDir = h->rayDirRaw * (1.f / dlen);
    h->reverseOrientation = tri->ReverseOrientation;

    // Per-level intermediate corner positions.  The mesh-owner-space corner
    // is the fixed ObjectToWorld applied to source P; each instance level
    // applies its local-to-parent affine.
    int n = (int)h->chain.size();
    h->levelPoints.resize(n + 1);
    h->levelPoints[n].resize(3);
    for (int k = 0; k < 3; ++k)
        h->levelPoints[n][k] = h->meshO2W.ApplyPoint(h->sourceP[k]);
    for (int l = n - 1; l >= 0; --l) {
        h->levelPoints[l].resize(3);
        for (int k = 0; k < 3; ++k)
            h->levelPoints[l][k] =
                h->chain[l].localToParent.ApplyPoint(
                    h->levelPoints[l + 1][k]);
    }
    for (int k = 0; k < 3; ++k) h->worldV[k] = h->levelPoints[0][k];

    if (!SolveTriangle(h->rayOrigin, h->rayDir, h->worldV,
                       &h->b1b2.x, &h->b1b2.y, &h->tHit)) {
        *err = "internal error: recorded hit failed to re-solve";
        return false;
    }

    bool isMirror;
    if (!ValidateMaterial(gp->ReplayMaterial(), &isMirror, h->rho, err))
        return false;
    h->isMirror = isMirror;

    // Keep pbrt's own hit parameter and ray epsilon; epsilon is treated as
    // fixed in the adjoint (visibility switching is not differentiated).
    h->pbrtTHit = pbrtTHit;
    h->rayEpsilon = isect.rayEpsilon;

    FinalizeHit(h);
    return true;
}

void EvaluateDirectLighting(const Scene *scene, HitRecord *h) {
    h->lights.clear();
    for (uint32_t li = 0; li < scene->lights.size(); ++li) {
        const PointLight *pl =
            static_cast<const PointLight *>(scene->lights[li]);
        HitRecord::LightTerm term;
        term.lightPos = V3(pl->ReplayLightPosition());
        Spectrum I = pl->ReplayIntensity();
        I.ToRGB(term.intensity);
        term.delta = term.lightPos - h->p;
        term.dist2 = LengthSquared(term.delta);
        float len = sqrtf(term.dist2);
        term.toLight = term.delta * (1.f / len);
        float ndl = Dot(h->geomNormal, term.toLight);
        term.cosine = ndl > 0.f ? ndl : 0.f;
        term.rayEpsilon = h->rayEpsilon;
        // Reproduce VisibilityTester::SetSegment + Scene::IntersectP.
        Ray shadow(h->p.AsPoint(), term.toLight.AsVector(),
                   term.rayEpsilon, len, 0.f);
        term.visible = !scene->IntersectP(shadow);
        h->lights.push_back(term);
    }
}

// Per-channel mirror weight accumulated along a path.
void MirrorWeight(const RayRecord &rr, float weight[3]) {
    weight[0] = weight[1] = weight[2] = 1.f;
    for (uint32_t i = 0; i < rr.hits.size(); ++i) {
        if (!rr.hits[i].isMirror) break;
        for (int c = 0; c < 3; ++c) weight[c] *= rr.hits[i].rho[c];
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Forward entry point.
// ---------------------------------------------------------------------------
bool ReplayTrace(const Scene *scene, const vector<ReplayRay> &rays,
                 int maxBounces, ReplayRecord **recordOut,
                 string *errorText) {
    string err;
    SceneTopology topo;
    if (!BuildTopology(scene, &topo, &err)) {
        if (errorText) *errorText = err;
        return false;
    }
    if (maxBounces < 0) {
        if (errorText) *errorText = "maxBounces must be non-negative";
        return false;
    }

    ReplayRecord *rec = new ReplayRecord();
    rec->rays.resize(rays.size());
    rec->colors.resize(rays.size());

    // Snapshot parameter block layout from the validated topology.  A shared
    // prototype contributes once per enclosing instance chain.
    map<int, ReplayRecord::BlockInfo> blockMap;
    for (map<const GeometricPrimitive *,
             vector<ResolverEntry> >::const_iterator eit =
             topo.entries.begin(); eit != topo.entries.end(); ++eit) {
        const GeometricPrimitive *gp = eit->first;
        const Triangle *tri =
            static_cast<const Triangle *>(gp->ReplayShape());
        const TriangleMesh *mesh = tri->ReplayMesh();
        map<const TriangleMesh *, int>::const_iterator mit =
            MeshBindings().find(mesh);
        if (mit != MeshBindings().end()) {
            ReplayRecord::BlockInfo &bi = blockMap[mit->second];
            bi.paramId = mit->second;
            bi.kind = REPLAY_PARAM_POSITION;
            int count = mesh->ReplayNumVertices() * 3;
            if (bi.count == 0) bi.count = count;
            else if (bi.count != count) {
                if (errorText)
                    *errorText = "meshes sharing one parameter ID must have "
                                 "matching vertex counts";
                delete rec;
                return false;
            }
        }
        for (uint32_t k = 0; k < eit->second.size(); ++k) {
            const vector<InstanceLevel> &chain = eit->second[k].chain;
            for (uint32_t j = 0; j < chain.size(); ++j) {
                if (chain[j].paramId < 0) continue;
                ReplayRecord::BlockInfo &bi = blockMap[chain[j].paramId];
                bi.paramId = chain[j].paramId;
                bi.kind = REPLAY_PARAM_MATRIX;
                if (bi.count == 0) bi.count = 12;
            }
        }
    }
    for (map<int, ReplayRecord::BlockInfo>::iterator it = blockMap.begin();
         it != blockMap.end(); ++it)
        rec->blocks.push_back(it->second);

    for (uint32_t r = 0; r < rays.size(); ++r) {
        RayRecord &rr = rec->rays[r];
        V3 o(rays[r].o), d(rays[r].d);
        if (!FiniteV3(o) || !FiniteV3(d) || LengthSquared(d) == 0.f) {
            if (errorText)
                *errorText = "ray origin or direction is not finite/non-zero";
            delete rec;
            return false;
        }
        rr.origin = o;
        rr.dir = d * (1.f / sqrtf(LengthSquared(d)));
        rr.black = false;

        Ray ray(o.AsPoint(), rr.dir.AsVector(), 0.f, INFINITY, 0.f);
        int bounces = 0;
        while (true) {
            Intersection isect;
            bool hit = scene->Intersect(ray, &isect);
            if (!hit) {
                rr.black = true;       // black background
                break;
            }
            float hitT = ray.maxt;     // pbrt stores the hit distance here
            HitRecord h;
            if (!BuildHitRecord(topo, isect, ray, hitT, &h, &err)) {
                if (errorText) *errorText = err;
                delete rec;
                return false;
            }
            if (h.isMirror) {
                rr.hits.push_back(h);
                if (bounces >= maxBounces) {
                    rr.black = true;   // bounce budget exceeded => black
                    break;
                }
                ++bounces;
                // Same spawn rule as SpecularReflect in the core integrator.
                ray = Ray(h.p.AsPoint(), h.reflectDir.AsVector(),
                          isect.rayEpsilon, INFINITY, 0.f);
                continue;
            }
            // Matte terminal: per-point-light Lambert direct lighting.
            EvaluateDirectLighting(scene, &h);
            rr.hits.push_back(h);
            break;
        }

        if (rr.black) {
            rr.color = ReplayColor();
            rec->colors[r] = rr.color;
            continue;
        }

        // Assemble linear RGB from the recorded path.
        float weight[3];
        MirrorWeight(rr, weight);
        const HitRecord &term = rr.hits.back();
        for (int c = 0; c < 3; ++c) {
            float sum = 0.f;
            for (uint32_t j = 0; j < term.lights.size(); ++j) {
                const HitRecord::LightTerm &L = term.lights[j];
                if (!L.visible) continue;
                sum += L.intensity[c] * L.cosine / L.dist2;
            }
            rr.color.rgb[c] =
                weight[c] * (term.rho[c] * INV_PI) * sum;
        }
        rec->colors[r] = rr.color;
    }

    *recordOut = rec;
    return true;
}

int ReplayNumRays(const ReplayRecord *record) {
    return record ? (int)record->rays.size() : 0;
}

const ReplayColor *ReplayColors(const ReplayRecord *record) {
    return record && !record->colors.empty() ? &record->colors[0] : NULL;
}

void ReplayReleaseRecord(ReplayRecord *record) {
    delete record;
}

// ---------------------------------------------------------------------------
// Hand-written adjoint.
// ---------------------------------------------------------------------------
namespace {

struct GradientStore {
    map<int, vector<float> > position;
    map<int, vector<float> > matrix;

    void AddPosition(int id, int vertex, const V3 &g,
                     const map<int, int> &counts) {
        if (id < 0) return;
        map<int, vector<float> >::iterator it = position.find(id);
        if (it == position.end()) {
            it = position.insert(std::make_pair(id,
                vector<float>(counts.find(id)->second, 0.f))).first;
        }
        it->second[3 * vertex + 0] += g.x;
        it->second[3 * vertex + 1] += g.y;
        it->second[3 * vertex + 2] += g.z;
    }
    void AddMatrix(int id, const float g[12]) {
        if (id < 0) return;
        map<int, vector<float> >::iterator it = matrix.find(id);
        if (it == matrix.end())
            it = matrix.insert(std::make_pair(id, vector<float>(12, 0.f))).first;
        for (int i = 0; i < 12; ++i) it->second[i] += g[i];
    }
};

// VJP of normalize(y = x/|x|).
inline V3 NormalizeVJP(const V3 &x, const V3 &y, const V3 &gy) {
    float len = sqrtf(LengthSquared(x));
    return (gy - y * Dot(gy, y)) * (1.f / len);
}

// Pulls world-space vertex adjoints back through the instance chain and the
// fixed mesh ObjectToWorld, accumulating matrix gradients per bound level
// and source-P gradients per bound mesh.  The point rule y = A x + t has
// VJPs g_A = g_y x^T, g_t = sum_k g_y_k and g_x = A^T g_y.
void PropagateVertexGradients(const HitRecord &h,
                              const V3 worldGrad[3],
                              GradientStore *store,
                              const map<int, int> &positionCounts) {
    int n = (int)h.chain.size();
    V3 cur[3] = { worldGrad[0], worldGrad[1], worldGrad[2] };
    for (int l = 0; l < n; ++l) {
        const Affine &A = h.chain[l].localToParent;
        const vector<V3> &inputPts = h.levelPoints[l + 1];
        float gMat[12] = { 0.f };
        V3 next[3];
        for (int k = 0; k < 3; ++k) {
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j)
                    gMat[4 * i + j] += cur[k][i] * inputPts[k][j];
                gMat[4 * i + 3] += cur[k][i];
            }
            for (int j = 0; j < 3; ++j) {
                float acc = 0.f;
                for (int i = 0; i < 3; ++i)
                    acc += A.m[4 * i + j] * cur[k][i];
                next[k][j] = acc;
            }
        }
        store->AddMatrix(h.chain[l].paramId, gMat);
        for (int k = 0; k < 3; ++k) cur[k] = next[k];
    }
    // cur now lives in mesh-owner space; mesh points = M0 * source P.
    for (int k = 0; k < 3; ++k) {
        V3 gSource;
        for (int j = 0; j < 3; ++j) {
            float acc = 0.f;
            for (int i = 0; i < 3; ++i)
                acc += h.meshO2W.m[4 * i + j] * cur[k][i];
            gSource[j] = acc;
        }
        store->AddPosition(h.meshParamId, h.vertIndex[k], gSource,
                           positionCounts);
    }
}

// Adjoint of one full hit.  adjOutOrigin/adjOutDir are the adjoints of the
// ray spawned at this hit (zero for a matte terminal); the spawn rule is
// o_next = p + epsilon * r, d_next = r with epsilon fixed.  The adjoints on
// the incoming ray are written to adjInOrigin/adjInDir.  All geometry
// adjoints are pulled back to source-P and instance-matrix parameters via
// PropagateVertexGradients().
void HitBackward(const HitRecord &h, bool isTerminal,
                 const float terminalSeed[3],
                 const V3 &adjOutOrigin, const V3 &adjOutDir,
                 V3 *adjInOrigin, V3 *adjInDir,
                 GradientStore *store,
                 const map<int, int> &positionCounts) {
    // p feeds the outgoing-ray origin o_next = p + epsilon*r with unit
    // Jacobian (epsilon is fixed and carries no derivative).
    V3 adjP = adjOutOrigin;
    V3 adjN;
    V3 adjRayO, adjRayD;

    if (isTerminal) {
        // RGB_c = W_c * rho_c/pi * sum_j I_jc * vis * c_j / d2_j; the
        // weight W, rho and I are constants, so only c and d2 differentiate.
        for (uint32_t j = 0; j < h.lights.size(); ++j) {
            const HitRecord::LightTerm &L = h.lights[j];
            if (!L.visible) continue;
            float k = 0.f;
            for (int c = 0; c < 3; ++c)
                k += terminalSeed[c] * L.intensity[c];
            if (k == 0.f) continue;
            float invD2 = 1.f / L.dist2;
            // q = c / d2.
            float adjC = k * invD2;
            float adjD2 = -k * L.cosine * invD2 * invD2;
            // d2 = |delta|^2, Lhat = normalize(delta).
            V3 adjDelta = L.delta * (2.f * adjD2);
            // c = max(0, N . Lhat): zero derivative on the c == 0 branch.
            if (L.cosine > 0.f) {
                adjN = adjN + L.toLight * adjC;
                V3 adjLhat = h.geomNormal * adjC;
                adjDelta = adjDelta +
                    NormalizeVJP(L.delta, L.toLight, adjLhat);
            }
            // delta = lightPos - p, lightPos constant.
            adjP = adjP - adjDelta;
        }
    } else {
        // Mirror: r = normalize(u), u = d - 2*alpha*N, alpha = d . N.
        float alphaDot = Dot(h.rayDir, h.geomNormal);
        V3 u = h.rayDir - h.geomNormal * (2.f * alphaDot);
        // r feeds d_next directly and o_next through epsilon*r.
        V3 adjR = adjOutDir + adjOutOrigin * h.rayEpsilon;
        V3 adjU = NormalizeVJP(u, h.reflectDir, adjR);
        // adj_d = adj_u - 2 N (N . adj_u)
        float nDotAdjU = Dot(h.geomNormal, adjU);
        adjRayD = adjU - h.geomNormal * (2.f * nDotAdjU);
        // adj_N = -2 (alpha adj_u + d (N . adj_u))
        adjN = (adjU * (-2.f * alphaDot)) -
               h.rayDir * (2.f * nDotAdjU);
    }

    // N = normalSign * normalize(face), face = e1 x e2.
    V3 e1 = h.worldV[1] - h.worldV[0];
    V3 e2 = h.worldV[2] - h.worldV[0];
    V3 face = Cross(e1, e2);
    V3 adjFaceN = adjN * h.normalSign;
    V3 adjFace = NormalizeVJP(face, h.faceNormal, adjFaceN);
    // VJPs of face = e1 x e2.
    V3 adjE1 = Cross(e2, adjFace);
    V3 adjE2 = Cross(adjFace, e1);

    // p = b0 v0 + b1 v1 + b2 v2, b0 = 1 - b1 - b2.
    float b1 = h.b1b2.x, b2 = h.b1b2.y, b0 = 1.f - b1 - b2;
    V3 adjV[3];
    adjV[0] = adjP * b0;
    adjV[1] = adjP * b1;
    adjV[2] = adjP * b2;
    float adjB1 = Dot(adjP, h.worldV[1] - h.worldV[0]);
    float adjB2 = Dot(adjP, h.worldV[2] - h.worldV[0]);

    // Moller-Trumbore VJP.  t feeds only the fixed ray epsilon, so adj_t = 0.
    //   s1 = d x e2; divisor = s1 . e1; inv = 1/divisor;
    //   s  = o - v0; s2 = s x e1; b1 = (s.s1)*inv; b2 = (d.s2)*inv.
    V3 s1 = Cross(h.rayDir, e2);
    float divisor = Dot(s1, e1);
    float inv = 1.f / divisor;
    V3 s = h.rayOrigin - h.worldV[0];
    V3 s2 = Cross(s, e1);
    float adjInv = adjB1 * Dot(s, s1) + adjB2 * Dot(h.rayDir, s2);
    // Fold the divisor adjoint into s1/e1 BEFORE using adjS1 downstream;
    // otherwise the contribution of adjS1 through the s1 = d x e2 cross
    // product would be dropped.
    float adjDivisor = -adjInv * inv * inv;
    V3 adjS1 = s * (adjB1 * inv) + e1 * adjDivisor;
    V3 adjS = s1 * (adjB1 * inv);
    V3 adjS2 = h.rayDir * (adjB2 * inv);
    adjE1 = adjE1 + s1 * adjDivisor;
    // s1 = d x e2.
    adjRayD = adjRayD + Cross(e2, adjS1);
    adjE2 = adjE2 + Cross(adjS1, h.rayDir);
    // s2 = s x e1.
    adjS = adjS + Cross(e1, adjS2);
    adjE1 = adjE1 + Cross(adjS2, s);
    adjRayD = adjRayD + s2 * (adjB2 * inv);
    // s = o - v0.
    adjRayO = adjRayO + adjS;
    adjV[0] = adjV[0] - adjS;
    // e1 = v1 - v0, e2 = v2 - v0.
    adjV[1] = adjV[1] + adjE1;
    adjV[0] = adjV[0] - adjE1;
    adjV[2] = adjV[2] + adjE2;
    adjV[0] = adjV[0] - adjE2;

    *adjInOrigin = adjRayO;
    *adjInDir = adjRayD;
    PropagateVertexGradients(h, adjV, store, positionCounts);
}} // anonymous namespace

ReplayGradient *ReplayBackward(const ReplayRecord *record,
                                const vector<ReplayColor> &adjoint) {
    ReplayGradient *out = new ReplayGradient();
    out->numBlocks = (int)record->blocks.size();
    out->blocks = new ReplayGradientBlock[out->numBlocks > 0 ?
                                          out->numBlocks : 1];
    map<int, int> positionCounts;
    for (int i = 0; i < out->numBlocks; ++i) {
        const ReplayRecord::BlockInfo &bi = record->blocks[i];
        out->blocks[i].parameterId = bi.paramId;
        out->blocks[i].kind = bi.kind;
        out->blocks[i].count = bi.count;
        out->blocks[i].values = new float[bi.count]();
        if (bi.kind == REPLAY_PARAM_POSITION)
            positionCounts[bi.paramId] = bi.count;
    }

    GradientStore store;
    int nr = (int)record->rays.size();
    for (int r = 0; r < nr; ++r) {
        const RayRecord &rr = record->rays[r];
        if (rr.black || rr.hits.empty()) continue;
        if ((int)adjoint.size() <= r) continue;

        // Per-channel path mirror weight; the terminal seed folds W*rho/pi in.
        float weight[3];
        MirrorWeight(rr, weight);
        const HitRecord &term = rr.hits.back();
        float seed[3];
        for (int c = 0; c < 3; ++c)
            seed[c] = adjoint[r].rgb[c] * weight[c] *
                      term.rho[c] * INV_PI;

        // Reverse sweep.  adjOutOrigin/adjOutDir hold the adjoints of the ray
        // spawned by the current hit; a matte terminal spawns no ray.
        V3 adjOutOrigin, adjOutDir;
        int last = (int)rr.hits.size() - 1;
        for (int i = last; i >= 0; --i) {
            const HitRecord &h = rr.hits[i];
            V3 adjInOrigin, adjInDir;
            float zeroSeed[3] = { 0.f, 0.f, 0.f };
            HitBackward(h, i == last, i == last ? seed : zeroSeed,
                        adjOutOrigin, adjOutDir,
                        &adjInOrigin, &adjInDir,
                        &store, positionCounts);
            if (i > 0) {
                // This hit's stored unit direction is the normalize() of the
                // direction spawned by the previous hit.  Push the direction
                // adjoint through that normalize; the spawn origin (p plus
                // the fixed epsilon) is handled inside HitBackward.
                adjOutDir = NormalizeVJP(h.rayDirRaw, h.rayDir, adjInDir);
                adjOutOrigin = adjInOrigin;
            }
        }
        // The remaining adjoints refer to the fixed input ray and are
        // intentionally discarded.
    }

    for (int i = 0; i < out->numBlocks; ++i) {
        ReplayGradientBlock &b = out->blocks[i];
        if (b.kind == REPLAY_PARAM_POSITION) {
            map<int, vector<float> >::const_iterator it =
                store.position.find(b.parameterId);
            if (it != store.position.end())
                for (int k = 0; k < b.count; ++k)
                    b.values[k] = it->second[k];
        } else {
            map<int, vector<float> >::const_iterator it =
                store.matrix.find(b.parameterId);
            if (it != store.matrix.end())
                for (int k = 0; k < b.count; ++k)
                    b.values[k] = it->second[k];
        }
    }
    return out;
}

ReplayGradientBlock *ReplayGradient::Find(int parameterId) {
    for (int i = 0; i < numBlocks; ++i)
        if (blocks[i].parameterId == parameterId) return &blocks[i];
    return NULL;
}

const ReplayGradientBlock *ReplayGradient::Find(int parameterId) const {
    for (int i = 0; i < numBlocks; ++i)
        if (blocks[i].parameterId == parameterId) return &blocks[i];
    return NULL;
}

void ReplayFreeGradient(ReplayGradient *gradient) {
    if (!gradient) return;
    for (int i = 0; i < gradient->numBlocks; ++i)
        delete[] gradient->blocks[i].values;
    delete[] gradient->blocks;
    delete gradient;
}
