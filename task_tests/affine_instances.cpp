// Affine instance regression: shared prototypes placed with finite,
// invertible affine transforms (non-uniform scale, shear, mirror).
//
// Every rendered result is checked against an independent reference built by
// explicitly transforming a small mesh with plain float matrix math. A
// negative-determinant reference flips triangle winding (never normals) so it
// keeps the prototype's front face, matching the engine's mirror convention.
//
// Registered through task_support/check.h only; this translation unit has no
// main() — task_support/driver.c supplies the runner.

#include "Scene.hpp"
#include "check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using namespace Renderer;

namespace {

constexpr int W = 160;
constexpr int H = 120;
constexpr int PIXELS = W * H;

// ---------------------------------------------------------------------------
// Independent 3x3 matrix reference (row-major, rows act on column vectors).
// ---------------------------------------------------------------------------
struct Mat3 {
    float m[9];
};

float det3(const Mat3& a) {
    const float* m = a.m;
    return m[0] * (m[4] * m[8] - m[5] * m[7])
         - m[1] * (m[3] * m[8] - m[5] * m[6])
         + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

bool orthogonal3(const Mat3& a) {
    constexpr float eps = 2.0e-3f;
    for (int r = 0; r < 3; ++r) {
        const float* p = a.m + r * 3;
        if (std::fabs(p[0] * p[0] + p[1] * p[1] + p[2] * p[2] - 1.0f) > eps)
            return false;
        for (int s = r + 1; s < 3; ++s) {
            const float* q = a.m + s * 3;
            if (std::fabs(p[0] * q[0] + p[1] * q[1] + p[2] * q[2]) > eps)
                return false;
        }
    }
    return true;
}

// Inverse transpose, evaluated through the same cofactor expressions and
// operation order as Scene.cpp's affine instance normal path.
Mat3 inverseTranspose3(const Mat3& a) {
    const float* m = a.m;
    const float c[9] = {
        m[4]*m[8]-m[5]*m[7], m[5]*m[6]-m[3]*m[8], m[3]*m[7]-m[4]*m[6],
        m[2]*m[7]-m[1]*m[8], m[0]*m[8]-m[2]*m[6], m[1]*m[6]-m[0]*m[7],
        m[1]*m[5]-m[2]*m[4], m[2]*m[3]-m[0]*m[5], m[0]*m[4]-m[1]*m[3]};
    const float invDet = 1.0f / det3(a);
    Mat3 r;
    for (int i = 0; i < 9; ++i) r.m[i] = c[i] * invDet;
    return r;
}

// Integer owner-local position, truncating the float result exactly like the
// renderer's (int32_t) cast after its instance mat-vec.
Vector3 applyPosition(const Mat3& a, const Vector3& p, const Vector3& t) {
    const float* m = a.m;
    const float x = (float)p.x, y = (float)p.y, z = (float)p.z;
    return Vector3(
        (int32_t)(m[0] * x + m[1] * y + m[2] * z) + t.x,
        (int32_t)(m[3] * x + m[4] * y + m[5] * z) + t.y,
        (int32_t)(m[6] * x + m[7] * y + m[8] * z) + t.z);
}

// Reference transformed normal. Orthogonal bases reuse the plain matrix
// (legacy path, unit length preserved); affine bases use M^-T followed by
// renormalization to FIXED_POINT_SCALE, mirroring the engine's per-vertex
// float expression order so quantized components match.
Vector3 applyNormal(const Mat3& a, const Vector3& n, bool orthogonal) {
    const Mat3 inverseTranspose = inverseTranspose3(a);
    const float* b = (orthogonal ? a.m : inverseTranspose.m);
    const float fnx = (float)n.x, fny = (float)n.y, fnz = (float)n.z;
    float gx = fnx * b[0] + fny * b[1] + fnz * b[2];
    float gy = fnx * b[3] + fny * b[4] + fnz * b[5];
    float gz = fnx * b[6] + fny * b[7] + fnz * b[8];
    if (!orthogonal) {
        const float len = std::sqrt(gx * gx + gy * gy + gz * gz);
        if (len > 0.0f) {
            const float inv = (float)FIXED_POINT_SCALE / len;
            gx *= inv; gy *= inv; gz *= inv;
        }
    }
    return Vector3((int32_t)gx, (int32_t)gy, (int32_t)gz);
}

Object::InstanceTransform toTransform(const Mat3& a, const Vector3& t = {0, 0, 0}) {
    Object::InstanceTransform transform;
    for (int i = 0; i < 9; ++i) transform.basis[i] = a.m[i];
    transform.position = t;
    return transform;
}

// ---------------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------------
// Two triangles sharing no vertices, with deliberately different materials
// and explicit flat normals. All coordinates are multiples of 20 so the
// rational scale/shear factors below land on integer owner-local positions.
Object::SharedMesh makePrototype(Material& matA, Material& matB) {
    Object authored;
    struct Face { Vector3 a, b, c; };
    const Face faces[2] = {
        {{-40, -40, 0}, {40, -40, 0}, {0, 40, 20}},
        {{-40, 20, -20}, {20, 20, -20}, {-20, -40, 40}},
    };
    for (int f = 0; f < 2; ++f) {
        const Vector3 u = faces[f].b - faces[f].a;
        const Vector3 v = faces[f].c - faces[f].a;
        float nx = (float)(u.y * v.z - u.z * v.y);
        float ny = (float)(u.z * v.x - u.x * v.z);
        float nz = (float)(u.x * v.y - u.y * v.x);
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        nx = nx * (float)FIXED_POINT_SCALE / len;
        ny = ny * (float)FIXED_POINT_SCALE / len;
        nz = nz * (float)FIXED_POINT_SCALE / len;
        const Vector3 normal((int32_t)nx, (int32_t)ny, (int32_t)nz);
        const uint16_t base = (uint16_t)authored.vertices.size();
        for (const Vector3& p : {faces[f].a, faces[f].b, faces[f].c})
            authored.addVertex({p, {0, 0}, normal});
        authored.addTriangle(base, base + 1, base + 2, f == 0 ? &matA : &matB);
    }
    return Object::freezeMesh(std::move(authored));
}

// Explicitly transformed ordinary-mesh reference. Negative-determinant
// placements swap triangle winding so the prototype's front face survives;
// normals keep pointing to the prototype's side (M^-T already carries the
// sign), they are never flipped manually.
Object expandReference(const Object& proto, const Mat3& basis,
                       const Vector3& translation, Material* overrideMat) {
    Object out;
    const bool orthogonal = orthogonal3(basis);
    const bool mirror = det3(basis) < 0.0f;
    for (const auto& vertex : proto.vertices) {
        Object::Vertex copy = vertex;
        copy.position = applyPosition(basis, vertex.position, translation);
        copy.normal = applyNormal(basis, vertex.normal, orthogonal);
        out.addVertex(copy);
    }
    for (const auto& tri : proto.triangles) {
        Object::Triangle copy = tri;
        if (mirror) std::swap(copy.v2, copy.v3);
        if (overrideMat) { copy.material = overrideMat; copy.colorBaked = false; }
        out.triangles.push_back(copy);
    }
    out.calculateBoundingBox();
    return out;
}

struct FrameBuffers {
    std::vector<uint16_t> color;
    std::vector<uint16_t> depth;
    FrameBuffers() : color(PIXELS, 0), depth(PIXELS, 65535) {}
    void clear() {
        std::fill(color.begin(), color.end(), 0);
        std::fill(depth.begin(), depth.end(), 65535);
    }
};

void renderFrame(Scene& scene, FrameBuffers& fb) {
    fb.clear();
    scene.prepareFrame();
    scene.rasterizeBand(0, H);
}

bool channelClose(uint16_t a, uint16_t b, int tolerance) {
    const int dr = std::abs(int((a >> 11) & 0x1F) - int((b >> 11) & 0x1F));
    const int dg = std::abs(int((a >> 5) & 0x3F) - int((b >> 5) & 0x3F));
    const int db = std::abs(int(a & 0x1F) - int(b & 0x1F));
    return dr <= tolerance && dg <= tolerance && db <= tolerance;
}

// tolerance = 0 requires pixel-identical output; 1 allows one RGB565
// quantization step per channel.
bool framesMatch(const FrameBuffers& got, const FrameBuffers& expected,
                 int tolerance, unsigned* mismatches) {
    *mismatches = 0;
    for (int i = 0; i < PIXELS; ++i)
        if (!channelClose(got.color[i], expected.color[i], tolerance))
            ++*mismatches;
    return *mismatches == 0;
}

// ---------------------------------------------------------------------------
// placement group
// ---------------------------------------------------------------------------
int placementValidation() {
    Material matA(0xF800), matB(0x07E0);
    auto proto = makePrototype(matA, matB);
    TASK_CHECK(proto != nullptr);
    TASK_CHECK(proto->cachedPositions() != nullptr);
    const Vector3* cachedBefore = proto->cachedPositions();
    const auto originalTriangles = proto->triangles;

    Object batch;
    TASK_CHECK(batch.addInstance(proto, Object::InstanceTransform{}));
    const size_t beforeCount = batch.instances.size();
    batch.calculateBoundingBox();
    const Vector3 boxMin = batch.boundingBoxMin, boxMax = batch.boundingBoxMax;

    const float nan = std::nan("");
    const float inf = std::numeric_limits<float>::infinity();
    Object::InstanceTransform bad;
    bad.basis[0] = nan;
    TASK_CHECK(!batch.addInstance(proto, bad));
    bad = Object::InstanceTransform{}; bad.basis[4] = inf;
    TASK_CHECK(!batch.addInstance(proto, bad));
    bad = Object::InstanceTransform{}; bad.basis[8] = 0.0f; // singular
    TASK_CHECK(!batch.addInstance(proto, bad));
    bad = Object::InstanceTransform{};
    bad.basis[3] = 1.0f; bad.basis[0] = 0.0f; // rank-deficient duplicate row
    TASK_CHECK(!batch.addInstance(proto, bad));
    TASK_CHECK(batch.instances.size() == beforeCount);
    TASK_CHECK(batch.boundingBoxMin.x == boxMin.x && batch.boundingBoxMin.y == boxMin.y &&
               batch.boundingBoxMin.z == boxMin.z);
    TASK_CHECK(batch.boundingBoxMax.x == boxMax.x && batch.boundingBoxMax.y == boxMax.y &&
               batch.boundingBoxMax.z == boxMax.z);

    // setInstanceTransform rejects the same class of bad placements without
    // touching the existing instance, its bounds or material binding.
    Object::InstanceTransform good = batch.instances[0].transform;
    Object::InstanceTransform saved = good;
    bad = good; bad.basis[1] = nan;
    TASK_CHECK(!batch.setInstanceTransform(0, bad));
    bad = good; bad.basis[7] = 0.0f; bad.basis[1] = 1.0f; bad.basis[4] = 0.0f; // singular
    TASK_CHECK(!batch.setInstanceTransform(0, bad));
    TASK_CHECK(!batch.setInstanceTransform(99, good));
    for (int i = 0; i < 9; ++i)
        TASK_CHECK(batch.instances[0].transform.basis[i] == saved.basis[i]);
    TASK_CHECK(batch.boundingBoxMin.x == boxMin.x);

    // A valid replacement is accepted and refreshes the batch box itself.
    Mat3 farBasis{{1.5f, 0.25f, 0.0f,
                    0.0f, 0.75f, 0.0f,
                    0.5f, 0.0f, 1.25f}};
    TASK_CHECK(det3(farBasis) > 0.0f);
    TASK_CHECK(batch.setInstanceTransform(0, toTransform(farBasis, {3000, 0, 0})));
    TASK_CHECK(batch.instances.size() == beforeCount);
    bool cornerOutsideOldBox = false;
    for (int i = 0; i < 8; ++i) {
        const Vector3 corner(
            (i & 1) ? proto->boundingBoxMax.x : proto->boundingBoxMin.x,
            (i & 2) ? proto->boundingBoxMax.y : proto->boundingBoxMin.y,
            (i & 4) ? proto->boundingBoxMax.z : proto->boundingBoxMin.z);
        const Vector3 placed = applyPosition(farBasis, corner, {3000, 0, 0});
        if (placed.x < batch.boundingBoxMin.x || placed.x > batch.boundingBoxMax.x ||
            placed.y < batch.boundingBoxMin.y || placed.y > batch.boundingBoxMax.y ||
            placed.z < batch.boundingBoxMin.z || placed.z > batch.boundingBoxMax.z)
            cornerOutsideOldBox = true;
        if (placed.x < boxMin.x || placed.x > boxMax.x) cornerOutsideOldBox = true;
    }
    TASK_CHECK(cornerOutsideOldBox);

    // The shared prototype, its position cache and its topology are untouched.
    TASK_CHECK(proto->cachedPositions() == cachedBefore);
    TASK_CHECK(proto->triangles.size() == originalTriangles.size());
    for (size_t i = 0; i < originalTriangles.size(); ++i)
        TASK_CHECK(proto->triangles[i].v1 == originalTriangles[i].v1 &&
                   proto->triangles[i].v2 == originalTriangles[i].v2 &&
                   proto->triangles[i].v3 == originalTriangles[i].v3);
    TASK_CHECK(batch.instances[0].mesh.get() == proto.get());
    return 0;
}

int placementRender() {
    Material matA(0xF800), matB(0x07E0);
    auto proto = makePrototype(matA, matB);

    // Rational scale + shear + mirror; every transformed prototype coordinate
    // stays integral, so instance and reference geometry coincide exactly.
    const Mat3 basisA{{1.5f, 0.25f, 0.0f,
                        0.0f, 0.75f, 0.0f,
                        0.5f, 0.0f, 1.25f}};           // det > 0, sheared + scaled
    const Mat3 basisB{{-1.2f, 0.5f, 0.0f,
                         0.0f, 0.8f, 0.25f,
                         0.0f, 0.0f, 1.0f}};           // det < 0, mirrored
    TASK_CHECK(det3(basisA) > 0.0f && det3(basisB) < 0.0f);
    TASK_CHECK(!orthogonal3(basisA) && !orthogonal3(basisB));

    Object batch;
    TASK_CHECK(batch.addInstance(proto, toTransform(basisA, {0, 0, 0})));
    TASK_CHECK(batch.addInstance(proto, toTransform(basisB, {0, 0, 60})));
    TASK_CHECK(batch.instances[0].mesh.get() == batch.instances[1].mesh.get());
    batch.position = {0, 0, 700};
    batch.calculateBoundingBox();

    Object refA = expandReference(*proto, basisA, {0, 0, 0}, nullptr);
    Object refB = expandReference(*proto, basisB, {0, 0, 60}, nullptr);
    refA.position = refB.position = {0, 0, 700};

    Camera camera;
    camera.nearPlane = 100; camera.farPlane = 4000; camera.setFOV(70.0f, W);
    DirectionalLight light(Vector3(0, 0, 0), Color{255, 255, 255}, 255);
    AmbientLight ambient({40, 40, 40});

    FrameBuffers gotBuf, expBuf;
    Scene got(gotBuf.color.data(), gotBuf.depth.data(), W, H);
    Scene expected(expBuf.color.data(), expBuf.depth.data(), W, H);
    got.setCamera(&camera); expected.setCamera(&camera);
    got.setDirectionalLight(&light); expected.setDirectionalLight(&light);
    got.setAmbientLight(&ambient); expected.setAmbientLight(&ambient);
    got.addObject(&batch);
    expected.addObject(&refA); expected.addObject(&refB);

    renderFrame(expected, expBuf);
    renderFrame(got, gotBuf);
    TASK_CHECK(got.lastFrameDrawnTriangles == expected.lastFrameDrawnTriangles);
    TASK_CHECK(got.lastFrameDrawnTriangles > 0);
    unsigned mismatches = 0;
    TASK_CHECK(framesMatch(gotBuf, expBuf, 1, &mismatches));

    // Move an instance repeatedly beyond its previous box; each frame the new
    // placement must drive culling and match the moved reference.
    for (int step = 1; step <= 3; ++step) {
        const Vector3 translation(step * 900, 0, 0);
        TASK_CHECK(batch.setInstanceTransform(0, toTransform(basisA, translation)));
        refA.position = {translation.x, 0, 700};
        renderFrame(expected, expBuf);
        renderFrame(got, gotBuf);
        // First steps sit fully outside the identity-camera frustum.
        if (step >= 2) TASK_CHECK(got.lastFrameDrawnTriangles <
                                  (int)proto->triangles.size() * 2);
        mismatches = 0;
        TASK_CHECK(framesMatch(gotBuf, expBuf, 1, &mismatches));
    }
    TASK_CHECK(batch.setInstanceTransform(0, toTransform(basisA, {0, 0, 0})));
    refA.position = {0, 0, 700};

    // Affine placements combined with a non-identity OWNER rotation exercise
    // the composed baseM*M position matrix and baseM*M^-T normal matrix. The
    // expanded reference is an ordinary Object with the same Euler rotation,
    // so the engine applies the identical owner transform to both.
    batch.rotation = refA.rotation = refB.rotation = {25, -40, 15};
    renderFrame(expected, expBuf);
    renderFrame(got, gotBuf);
    TASK_CHECK(got.lastFrameDrawnTriangles == expected.lastFrameDrawnTriangles);
    mismatches = 0;
    TASK_CHECK(framesMatch(gotBuf, expBuf, 1, &mismatches));
    batch.rotation = refA.rotation = refB.rotation = {0, 0, 0};

    // Positive-determinant orthogonal rotation control: an expanded ordinary
    // mesh and the rotated shared instance must be pixel-identical, with no
    // quantization tolerance.
    Object rotated;
    TASK_CHECK(rotated.addInstance(
        proto, Object::InstanceTransform::rotated({0, 35, 0}, {0, 0, 0})));
    rotated.position = {0, 0, 700};
    Object expanded = expandReference(
        *proto,
        [&] {
            Mat3 r;
            const auto& b = Object::InstanceTransform::rotated({0, 35, 0}).basis;
            for (int i = 0; i < 9; ++i) r.m[i] = b[i];
            return r;
        }(),
        {0, 0, 0}, nullptr);
    expanded.position = {0, 0, 700};
    FrameBuffers rotBuf, flatBuf;
    Scene rotScene(rotBuf.color.data(), rotBuf.depth.data(), W, H);
    Scene flatScene(flatBuf.color.data(), flatBuf.depth.data(), W, H);
    rotScene.setCamera(&camera); flatScene.setCamera(&camera);
    rotScene.setDirectionalLight(&light); flatScene.setDirectionalLight(&light);
    rotScene.setAmbientLight(&ambient); flatScene.setAmbientLight(&ambient);
    rotScene.addObject(&rotated); flatScene.addObject(&expanded);
    renderFrame(flatScene, flatBuf);
    renderFrame(rotScene, rotBuf);
    mismatches = 0;
    TASK_CHECK(framesMatch(rotBuf, flatBuf, 0, &mismatches));

    // Negative-determinant ORTHOGONAL reflection control: the legacy
    // reflection convention is corrected so the prototype's front face is
    // preserved. The normal basis stays M (M^-T == M for orthogonal M), so
    // the mirrored instance and the winding-swapped ordinary reference must
    // also be pixel-identical with zero tolerance.
    const Mat3 reflection{{1.0f, 0.0f, 0.0f,
                           0.0f, 1.0f, 0.0f,
                           0.0f, 0.0f, -1.0f}};
    TASK_CHECK(orthogonal3(reflection) && det3(reflection) < 0.0f);
    Object mirrored;
    TASK_CHECK(mirrored.addInstance(proto, toTransform(reflection)));
    mirrored.position = {0, 0, 700};
    Object mirroredFlat = expandReference(*proto, reflection, {0, 0, 0}, nullptr);
    mirroredFlat.position = {0, 0, 700};
    FrameBuffers mirBuf, mirFlatBuf;
    Scene mirScene(mirBuf.color.data(), mirBuf.depth.data(), W, H);
    Scene mirFlatScene(mirFlatBuf.color.data(), mirFlatBuf.depth.data(), W, H);
    mirScene.setCamera(&camera); mirFlatScene.setCamera(&camera);
    mirScene.setDirectionalLight(&light); mirFlatScene.setDirectionalLight(&light);
    mirScene.setAmbientLight(&ambient); mirFlatScene.setAmbientLight(&ambient);
    mirScene.addObject(&mirrored); mirFlatScene.addObject(&mirroredFlat);
    renderFrame(mirFlatScene, mirFlatBuf);
    renderFrame(mirScene, mirBuf);
    TASK_CHECK(mirScene.lastFrameDrawnTriangles == mirFlatScene.lastFrameDrawnTriangles);
    TASK_CHECK(mirScene.lastFrameDrawnTriangles > 0);
    mismatches = 0;
    TASK_CHECK(framesMatch(mirBuf, mirFlatBuf, 0, &mismatches));
    return 0;
}

// ---------------------------------------------------------------------------
// lighting group
// ---------------------------------------------------------------------------
int lightingNormals() {
    Material matA(0xF800), matB(0x07E0);
    auto proto = makePrototype(matA, matB);
    const Mat3 basisA{{1.5f, 0.25f, 0.0f,
                        0.0f, 0.75f, 0.0f,
                        0.5f, 0.0f, 1.25f}};
    const Mat3 basisB{{-1.2f, 0.5f, 0.0f,
                         0.0f, 0.8f, 0.25f,
                         0.0f, 0.0f, 1.0f}};
    for (const Mat3* basis : {&basisA, &basisB}) {
        const bool mirror = det3(*basis) < 0.0f;
        const Mat3 invT = inverseTranspose3(*basis);
        Object ref = expandReference(*proto, *basis, {0, 0, 0}, nullptr);
        for (const auto& tri : proto->triangles) {
            const Vector3 n = ref.vertices[tri.v1].normal;
            // Renormalized back to FIXED_POINT_SCALE after quantization.
            const int32_t length = n.length();
            TASK_CHECK(length >= 1014 && length <= FIXED_POINT_SCALE);

            // The quantized normal stays perpendicular to both transformed
            // tangent edges of its face.
            const Vector3 a = ref.vertices[tri.v1].position;
            const Vector3 b = ref.vertices[tri.v2].position;
            const Vector3 c = ref.vertices[tri.v3].position;
            const Vector3 e1 = b - a, e2 = c - a;
            const int64_t d1 = Vector3::dotProduct(n, e1);
            const int64_t d2 = Vector3::dotProduct(n, e2);
            const int64_t span = int64_t(length) *
                std::max({std::abs(e1.x) + std::abs(e1.y) + std::abs(e1.z),
                          std::abs(e2.x) + std::abs(e2.y) + std::abs(e2.z), 1});
            TASK_CHECK(std::abs(d1) * 40 <= span);
            TASK_CHECK(std::abs(d2) * 40 <= span);

            // Side convention: the rendered normal must align with
            // M^-T * n0 (the prototype normal's side), while the raw
            // cofactor direction det(M) * M^-T * n0 faces the opposite way
            // when the placement mirrors (det < 0).
            const Vector3 n0 = proto->vertices[tri.v1].normal;
            const float f0x = (float)n0.x, f0y = (float)n0.y, f0z = (float)n0.z;
            float ix = invT.m[0] * f0x + invT.m[3] * f0y + invT.m[6] * f0z;
            float iy = invT.m[1] * f0x + invT.m[4] * f0y + invT.m[7] * f0z;
            float iz = invT.m[2] * f0x + invT.m[5] * f0y + invT.m[8] * f0z;
            const float sideDot =
                (float)n.x * ix + (float)n.y * iy + (float)n.z * iz;
            TASK_CHECK(sideDot > 0.0f);
            if (mirror) {
                const float cofactorDot = sideDot * det3(*basis);
                TASK_CHECK(cofactorDot < 0.0f);
            }
        }
    }
    return 0;
}

int lightingShading() {
    Material matA(0xF800), matB(0x39CF);
    auto proto = makePrototype(matA, matB);
    const Mat3 basisA{{1.5f, 0.25f, 0.0f,
                        0.0f, 0.75f, 0.0f,
                        0.5f, 0.0f, 1.25f}};
    const Mat3 basisB{{-1.2f, 0.5f, 0.0f,
                         0.0f, 0.8f, 0.25f,
                         0.0f, 0.0f, 1.0f}};
    Material overrideMat(0x4E9F);

    Object batch;
    TASK_CHECK(batch.addInstance(proto, toTransform(basisA)));
    TASK_CHECK(batch.addInstance(proto, toTransform(basisB, {0, 0, 60})));
    batch.position = {0, 0, 700};
    batch.calculateBoundingBox();

    Camera camera;
    camera.nearPlane = 100; camera.farPlane = 4000; camera.setFOV(70.0f, W);
    DirectionalLight light(Vector3(0, 0, 0), Color{255, 255, 255}, 255);
    AmbientLight ambient({40, 40, 40});

    FrameBuffers gotBuf, expBuf;
    Scene got(gotBuf.color.data(), gotBuf.depth.data(), W, H);
    Scene expected(expBuf.color.data(), expBuf.depth.data(), W, H);
    got.setCamera(&camera); expected.setCamera(&camera);
    got.setDirectionalLight(&light); expected.setDirectionalLight(&light);
    got.setAmbientLight(&ambient); expected.setAmbientLight(&ambient);
    got.addObject(&batch);

    // Flip shading/specular configuration and the material override between
    // frames. No precomputed lighting may survive a configuration where it
    // does not apply: every frame must match a freshly built reference.
    struct Mode { ShadingMode mode; uint8_t specular; uint8_t exponent; bool override; };
    const Mode modes[] = {
        {ShadingMode::FLAT, 0, 0, false},     // Lambert precompute candidate
        {ShadingMode::GOURAUD, 32, 0, false}, // view-space specular path
        {ShadingMode::PHONG, 48, 32, false},  // per-pixel normal/specular path
        {ShadingMode::FLAT, 0, 0, true},      // uniform material override
        {ShadingMode::GOURAUD, 64, 0, true},  // override + specular switch
        {ShadingMode::PHONG, 32, 16, true},   // override + per-pixel specular
        {ShadingMode::FLAT, 0, 0, false},     // back where we started
    };
    for (const Mode& mode : modes) {
        matA.shadingMode = matB.shadingMode = mode.mode;
        matA.specular = matB.specular = mode.specular;
        matA.specularExponent = matB.specularExponent = mode.exponent;
        overrideMat.shadingMode = mode.mode;
        overrideMat.specular = mode.specular;
        overrideMat.specularExponent = mode.exponent;
        batch.instances[0].materialOverride = mode.override ? &overrideMat : nullptr;

        std::vector<std::unique_ptr<Object>> refs;
        refs.push_back(std::make_unique<Object>(
            expandReference(*proto, basisA, {0, 0, 0},
                            mode.override ? &overrideMat : nullptr)));
        refs.push_back(std::make_unique<Object>(
            expandReference(*proto, basisB, {0, 0, 60}, nullptr)));
        for (auto& ref : refs) { ref->position = {0, 0, 700}; expected.addObject(ref.get()); }

        renderFrame(expected, expBuf);
        renderFrame(got, gotBuf);
        TASK_CHECK(got.lastFrameDrawnTriangles > 0);
        TASK_CHECK(got.lastFrameDrawnTriangles == expected.lastFrameDrawnTriangles);
        unsigned mismatches = 0;
        TASK_CHECK(framesMatch(gotBuf, expBuf, 1, &mismatches));

        for (auto& ref : refs) expected.getObjects().erase(
            std::find(expected.getObjects().begin(), expected.getObjects().end(), ref.get()));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// visibility group
// ---------------------------------------------------------------------------
int visibilityCullingAndClip() {
    Material matA(0xF800), matB(0x07E0);
    auto proto = makePrototype(matA, matB);
    const Mat3 mirror{{-1.2f, 0.5f, 0.0f,
                         0.0f, 0.8f, 0.25f,
                         0.0f, 0.0f, 1.0f}};
    TASK_CHECK(det3(mirror) < 0.0f);

    Camera camera;
    camera.nearPlane = 100; camera.farPlane = 4000; camera.setFOV(70.0f, W);
    DirectionalLight light(Vector3(0, 0, 0), Color{255, 255, 255}, 255);
    AmbientLight ambient({40, 40, 40});

    FrameBuffers gotBuf, expBuf;
    Scene got(gotBuf.color.data(), gotBuf.depth.data(), W, H);
    Scene expected(expBuf.color.data(), expBuf.depth.data(), W, H);
    got.setCamera(&camera); expected.setCamera(&camera);
    got.setDirectionalLight(&light); expected.setDirectionalLight(&light);
    got.setAmbientLight(&ambient); expected.setAmbientLight(&ambient);

    Object batch;
    TASK_CHECK(batch.addInstance(proto, toTransform(mirror, {0, 0, 0})));
    batch.position = {0, 0, 700};
    batch.calculateBoundingBox();
    Object ref = expandReference(*proto, mirror, {0, 0, 0}, nullptr);
    ref.position = {0, 0, 700};
    got.addObject(&batch); expected.addObject(&ref);

    // Mirroring must not swap front/back for any culling convention.
    const CullingMode modes[3] = {
        CullingMode::CULL_BACKFACES, CullingMode::CULL_FRONTFACES, CullingMode::NO_CULLING};
    for (CullingMode mode : modes) {
        batch.cullingMode = ref.cullingMode = mode;
        renderFrame(expected, expBuf);
        renderFrame(got, gotBuf);
        TASK_CHECK(got.lastFrameDrawnTriangles == expected.lastFrameDrawnTriangles);
        unsigned mismatches = 0;
        TASK_CHECK(framesMatch(gotBuf, expBuf, 1, &mismatches));
    }
    TASK_CHECK(got.lastFrameDrawnTriangles > 0);

    // Near-plane straddle: clipping, depth and the surviving triangles all
    // use the same instance placement as the explicit reference.
    Object nearBatch;
    TASK_CHECK(nearBatch.addInstance(proto, toTransform(mirror, {0, 0, 0})));
    nearBatch.position = {0, 0, 120};
    nearBatch.calculateBoundingBox();
    Object nearRef = expandReference(*proto, mirror, {0, 0, 0}, nullptr);
    nearRef.position = {0, 0, 120};
    Scene nearGot(gotBuf.color.data(), gotBuf.depth.data(), W, H);
    Scene nearExpected(expBuf.color.data(), expBuf.depth.data(), W, H);
    nearGot.setCamera(&camera); nearExpected.setCamera(&camera);
    nearGot.setDirectionalLight(&light); nearExpected.setDirectionalLight(&light);
    nearGot.setAmbientLight(&ambient); nearExpected.setAmbientLight(&ambient);
    nearGot.addObject(&nearBatch); nearExpected.addObject(&nearRef);
    renderFrame(nearExpected, expBuf);
    renderFrame(nearGot, gotBuf);
    TASK_CHECK(nearGot.lastFrameDrawnTriangles > 0);
    TASK_CHECK(nearGot.lastFrameDrawnTriangles == nearExpected.lastFrameDrawnTriangles);
    unsigned mismatches = 0;
    TASK_CHECK(framesMatch(gotBuf, expBuf, 1, &mismatches));

    // Batch cull: pushing the only placement past the far plane drops the
    // whole owner, and bringing it back restores the draw.
    TASK_CHECK(batch.setInstanceTransform(0, toTransform(mirror, {0, 0, 9000})));
    renderFrame(got, gotBuf);
    TASK_CHECK(got.lastFrameDrawnTriangles == 0);
    TASK_CHECK(batch.setInstanceTransform(0, toTransform(mirror, {0, 0, 0})));
    renderFrame(got, gotBuf);
    TASK_CHECK(got.lastFrameDrawnTriangles > 0);
    return 0;
}

#if MAX_PICK_QUERIES > 0
int visibilityPicking() {
    // Dedicated single-triangle prototype: any covered pixel necessarily
    // belongs to prototype triangle 0, so attribution is unambiguous.
    Material mat(0xF800);
    Object authored;
    authored.addVertex({{-50, -50, 0}, {0, 0}, {0, 0, -1024}});
    authored.addVertex({{50, -50, 0}, {0, 0}, {0, 0, -1024}});
    authored.addVertex({{0, 50, 0}, {0, 0}, {0, 0, -1024}});
    authored.addTriangle(0, 1, 2, &mat);
    auto proto = Object::freezeMesh(std::move(authored));
    const Mat3 basisA{{1.5f, 0.25f, 0.0f,
                        0.0f, 0.75f, 0.0f,
                        0.5f, 0.0f, 1.25f}};
    const Mat3 basisB{{-1.2f, 0.5f, 0.0f,
                         0.0f, 0.8f, 0.25f,
                         0.0f, 0.0f, 1.0f}};
    Object batch;
    // Separate the two placements horizontally so their projected triangles
    // occupy distinct screen regions; each pick pixel belongs to one instance.
    const Vector3 t0(-120, 0, 0), t1(120, 0, 0);
    TASK_CHECK(batch.addInstance(proto, toTransform(basisA, t0)));
    TASK_CHECK(batch.addInstance(proto, toTransform(basisB, t1)));
    batch.position = {0, 0, 700};
    batch.cullingMode = CullingMode::NO_CULLING;
    batch.calculateBoundingBox();

    Camera camera;
    camera.nearPlane = 100; camera.farPlane = 4000; camera.setFOV(70.0f, W);
    FrameBuffers fb;
    Scene scene(fb.color.data(), fb.depth.data(), W, H);
    scene.setCamera(&camera);
    scene.addObject(&batch);

    // Project the triangle centroid for a placement plus object position.
    auto project = [&](const Mat3& basis, const Vector3& translation,
                       const Vector3& objectPosition) {
        Vector3 centre{0, 0, 0};
        for (uint16_t idx : {uint16_t(0), uint16_t(1), uint16_t(2)}) {
            const Vector3 p = applyPosition(basis, proto->vertices[idx].position, translation);
            centre = centre + p;
        }
        centre = centre.divide(3) + objectPosition;
        const float invZ = camera.fovFactor / (float)centre.z;
        return Vector2((int32_t)((float)centre.x * invZ) + W / 2,
                       H / 2 - (int32_t)((float)centre.y * invZ));
    };
    const Vector2 p0 = project(basisA, t0, batch.position);
    const Vector2 p1 = project(basisB, t1, batch.position);
    PickQuery queries[2] = {{(int16_t)p0.x, (int16_t)p0.y},
                            {(int16_t)p1.x, (int16_t)p1.y}};
    scene.setPickQueries(queries, 2);
    renderFrame(scene, fb);
    const PickResult* results = scene.getPickResults();
    TASK_CHECK(results[0].hit);
    TASK_CHECK(results[0].object == &batch);
    TASK_CHECK(results[0].mesh == proto.get());
    TASK_CHECK(results[0].instanceIndex == 0);
    TASK_CHECK(results[0].triangleIndex == 0);
    TASK_CHECK(results[1].hit);
    TASK_CHECK(results[1].object == &batch);
    TASK_CHECK(results[1].mesh == proto.get());
    TASK_CHECK(results[1].instanceIndex == 1);
    TASK_CHECK(results[1].triangleIndex == 0);

    // A near-plane-straddling placement still attributes its surviving
    // triangles to the same prototype and instance indices.
    Object nearBatch;
    TASK_CHECK(nearBatch.addInstance(proto, toTransform(basisA, {0, 0, 0})));
    nearBatch.position = {0, 0, 120};
    nearBatch.cullingMode = CullingMode::NO_CULLING;
    nearBatch.calculateBoundingBox();
    Scene nearScene(fb.color.data(), fb.depth.data(), W, H);
    nearScene.setCamera(&camera);
    nearScene.addObject(&nearBatch);
    const Vector2 np = project(basisA, {0, 0, 0}, nearBatch.position);
    PickQuery nearQuery{(int16_t)np.x, (int16_t)np.y};
    nearScene.setPickQueries(&nearQuery, 1);
    renderFrame(nearScene, fb);
    const PickResult& nr = nearScene.getPickResults()[0];
    TASK_CHECK(nr.hit);
    TASK_CHECK(nr.object == &nearBatch && nr.mesh == proto.get());
    TASK_CHECK(nr.instanceIndex == 0 && nr.triangleIndex == 0);
    return 0;
}
#endif

int visibilityDepth() {
    Material near(0xF800), far(0x07E0);
    // Emissive keeps the raw material colour without any scene lighting.
    near.emissive = far.emissive = true;
    Object authored;
    authored.addVertex({{-60, -60, 0}, {0, 0}, {0, 0, -1024}});
    authored.addVertex({{60, -60, 0}, {0, 0}, {0, 0, -1024}});
    authored.addVertex({{0, 60, 0}, {0, 0}, {0, 0, -1024}});
    authored.addTriangle(0, 1, 2, &far);
    auto proto = Object::freezeMesh(std::move(authored));

    const Mat3 scaled{{1.25f, 0.0f, 0.0f,
                        0.0f, 1.25f, 0.0f,
                        0.25f, 0.0f, 1.0f}};
    Object batch;
    // Camera looks down +z, so a negative local-z offset is nearer.
    TASK_CHECK(batch.addInstance(proto, toTransform(scaled, {0, 0, -200}), &near));
    TASK_CHECK(batch.addInstance(proto, toTransform(scaled, {0, 0, 0}), &far));
    batch.position = {0, 0, 700};
    batch.cullingMode = CullingMode::NO_CULLING;
    batch.calculateBoundingBox();

    Camera camera;
    camera.nearPlane = 100; camera.farPlane = 4000; camera.setFOV(70.0f, W);
    FrameBuffers fb;
    Scene scene(fb.color.data(), fb.depth.data(), W, H);
    scene.setCamera(&camera);
    scene.addObject(&batch);
    renderFrame(scene, fb);
    TASK_CHECK(scene.lastFrameDrawnTriangles >= 2);
    // The nearer, sheared placement owns the centre pixel despite drawing
    // second; the z-buffer resolves both placements from one frame's data.
    TASK_CHECK(fb.color[(H / 2) * W + W / 2] == near.color);
    return 0;
}

#if TEXTURE_MAPPING
// Textured affine/mirror placements: the winding swap must carry the
// per-vertex UVs with it. The expanded reference keeps UVs on its vertices
// and swaps triangle indices, which is the same reordering the renderer
// performs on its UV pointers.
int placementTextured() {
    uint16_t texels[16];
    for (int i = 0; i < 16; ++i) texels[i] = (uint16_t)(0x1234 + i * 1771);
    Texture texture(4, 4, texels);
    Material textured(0xFFFF, &texture);
    textured.shadingMode = ShadingMode::UNLIT; // isolate texture sampling

    Object authored;
    authored.addVertex({{-50, -50, 0}, {0, 0}, {0, 0, -1024}});
    authored.addVertex({{50, -50, 0}, {2048, 0}, {0, 0, -1024}});
    authored.addVertex({{50, 50, 0}, {2048, 2048}, {0, 0, -1024}});
    authored.addVertex({{-50, 50, 0}, {0, 2048}, {0, 0, -1024}});
    authored.addFace(0, 1, 2, 3, &textured);
    auto proto = Object::freezeMesh(std::move(authored));
    TASK_CHECK(proto != nullptr);

    const Mat3 sheared{{1.5f, 0.25f, 0.0f,
                        0.0f, 0.75f, 0.0f,
                        0.5f, 0.0f, 1.25f}};
    const Mat3 mirror{{-1.2f, 0.5f, 0.0f,
                        0.0f, 0.8f, 0.25f,
                        0.0f, 0.0f, 1.0f}};

    Object batch;
    TASK_CHECK(batch.addInstance(proto, toTransform(sheared, {-70, 0, 0})));
    TASK_CHECK(batch.addInstance(proto, toTransform(mirror, {70, 0, 0})));
    batch.position = {0, 0, 700};
    batch.cullingMode = CullingMode::NO_CULLING;
    batch.calculateBoundingBox();
    Object refA = expandReference(*proto, sheared, {-70, 0, 0}, nullptr);
    Object refB = expandReference(*proto, mirror, {70, 0, 0}, nullptr);
    refA.position = refB.position = {0, 0, 700};
    refA.cullingMode = refB.cullingMode = CullingMode::NO_CULLING;

    Camera camera;
    camera.nearPlane = 100; camera.farPlane = 4000; camera.setFOV(70.0f, W);
    FrameBuffers gotBuf, expBuf;
    Scene got(gotBuf.color.data(), gotBuf.depth.data(), W, H);
    Scene expected(expBuf.color.data(), expBuf.depth.data(), W, H);
    got.setCamera(&camera); expected.setCamera(&camera);
    got.addObject(&batch);
    expected.addObject(&refA); expected.addObject(&refB);
    renderFrame(expected, expBuf);
    renderFrame(got, gotBuf);
    TASK_CHECK(got.lastFrameDrawnTriangles == expected.lastFrameDrawnTriangles);
    TASK_CHECK(got.lastFrameDrawnTriangles >= 2);
    unsigned mismatches = 0;
    TASK_CHECK(framesMatch(gotBuf, expBuf, 1, &mismatches));
    return 0;
}
#endif // TEXTURE_MAPPING

} // namespace

const task_case task_cases[] = {
    {"placement", "validation_and_bounds", placementValidation},
    {"placement", "rendered_placement", placementRender},
#if TEXTURE_MAPPING
    {"placement", "textured_placement", placementTextured},
#endif
    {"lighting", "normal_quantization", lightingNormals},
    {"lighting", "shading_and_override", lightingShading},
#if MAX_PICK_QUERIES > 0
    {"visibility", "culling_and_clipping", visibilityCullingAndClip},
    {"visibility", "pick_attribution", visibilityPicking},
#else
    {"visibility", "culling_and_clipping", visibilityCullingAndClip},
#endif
    {"visibility", "depth_resolution", visibilityDepth},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
