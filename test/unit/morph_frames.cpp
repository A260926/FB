// Unit checks for the optional morph target frame completion post-processing
// step (AI_CONFIG_PP_MORPH_FRAME_COMPLETION) and the glTF2 target tangent
// export switch (GLTF2_TARGET_TANGENT_EXP). Registered with the task driver
// in task_support/driver.c via task_support/check.h.
#include "task_support/check.h"

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

// rapidjson must be configured exactly like the library build
// (see code/CMakeLists.txt), otherwise the translation units violate ODR.
#ifndef RAPIDJSON_HAS_STDSTRING
#define RAPIDJSON_HAS_STDSTRING 1
#endif
#ifndef RAPIDJSON_NOMEMBERITERATORCLASS
#define RAPIDJSON_NOMEMBERITERATORCLASS 1
#endif
#include <rapidjson/document.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Allocation recording: proves that completed morph target frames are owned
// (and released) by their own aiAnimMesh, never by the base mesh or siblings.
// ---------------------------------------------------------------------------
static std::set<void *> &freedPointers() {
    static std::set<void *> *s = new std::set<void *>();
    return *s;
}
static bool g_recordFrees = false;
static void recordFree(void *p) {
    if (g_recordFrees && p) {
        freedPointers().insert(p);
    }
}
void *operator new(std::size_t n) {
    void *p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void *operator new[](std::size_t n) {
    void *p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void *p) noexcept { recordFree(p); std::free(p); }
void operator delete(void *p, std::size_t) noexcept { recordFree(p); std::free(p); }
void operator delete[](void *p) noexcept { recordFree(p); std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { recordFree(p); std::free(p); }

// ---------------------------------------------------------------------------
// Small hand-rolled vector math. The expected frames below are computed from
// first principles with these helpers, independently of the library code.
// ---------------------------------------------------------------------------
struct v3 {
    float x, y, z;
};
static v3 add3(v3 a, v3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static v3 sub3(v3 a, v3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static v3 mul3(v3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot3(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross3(v3 a, v3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static float len3(v3 a) { return std::sqrt(dot3(a, a)); }
static v3 norm3(v3 a) {
    float l = len3(a);
    return l > 0.0f ? mul3(a, 1.0f / l) : v3{0.0f, 0.0f, 0.0f};
}
static v3 toV3(const aiVector3D &v) { return {v.x, v.y, v.z}; }
static bool near3(const aiVector3D &a, v3 b, float eps = 1e-4f) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}
static bool near1(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Expected tangent frame of one isolated triangle, recomputed straight from
// the UV parameterization rule (project the UV axes onto the plane of the
// vertex normal and normalize), independent of the library implementation.
struct TriFrame {
    v3 n, t, b;
};
static TriFrame expectedTriFrame(v3 p0, v3 p1, v3 p2, const float uv[3][2]) {
    TriFrame f;
    v3 v = sub3(p1, p0), w = sub3(p2, p0);
    f.n = norm3(cross3(v, w));
    float sx = uv[1][0] - uv[0][0], sy = uv[1][1] - uv[0][1];
    float tx = uv[2][0] - uv[0][0], ty = uv[2][1] - uv[0][1];
    float dc = (tx * sy - ty * sx) < 0.0f ? -1.0f : 1.0f;
    if (sx * ty == sy * tx) {
        sx = 0.0f; sy = 1.0f; tx = 1.0f; ty = 0.0f;
    }
    v3 tf{(w.x * sy - v.x * ty) * dc, (w.y * sy - v.y * ty) * dc, (w.z * sy - v.z * ty) * dc};
    v3 bf{(w.x * sx - v.x * tx) * dc, (w.y * sx - v.y * tx) * dc, (w.z * sx - v.z * tx) * dc};
    f.t = norm3(sub3(tf, mul3(f.n, dot3(tf, f.n))));
    f.b = norm3(sub3(bf, mul3(f.n, dot3(bf, f.n))));
    return f;
}
static float handedness(v3 n, v3 t, v3 b) {
    return dot3(cross3(n, t), b) < 0.0f ? -1.0f : 1.0f;
}

// ---------------------------------------------------------------------------
// In-memory glTF 2.0 scene builder (single buffer, data URI). Morph target
// accessors hold diffs against the base frame, as required by the spec.
// ---------------------------------------------------------------------------
struct TargetSpec {
    std::string name;
    float weight = 0.0f;
    std::vector<v3> positions; // absolute endpoint positions, empty = absent
    std::vector<v3> normals;   // absolute endpoint normals, empty = absent
    std::vector<v3> tangents;  // absolute endpoint tangents (xyz), empty = absent
};
struct MeshSpec {
    std::vector<v3> positions;
    std::vector<v3> normals;                  // empty = no NORMAL attribute
    std::vector<std::array<float, 4>> tangents; // empty = no TANGENT attribute
    std::vector<std::array<float, 2>> uvs;    // empty = no TEXCOORD_0
    std::vector<TargetSpec> targets;
};

static std::string fnum(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

static std::string base64Encode(const std::vector<float> &data) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(data.data());
    size_t n = data.size() * sizeof(float);
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned int b0 = bytes[i];
        unsigned int b1 = (i + 1 < n) ? bytes[i + 1] : 0;
        unsigned int b2 = (i + 2 < n) ? bytes[i + 2] : 0;
        out += tbl[b0 >> 2];
        out += tbl[((b0 & 3) << 4) | (b1 >> 4)];
        out += (i + 1 < n) ? tbl[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        out += (i + 2 < n) ? tbl[b2 & 63] : '=';
    }
    return out;
}

static std::string buildGltf(const MeshSpec &spec) {
    std::vector<float> bin;
    std::string viewsJson, accessorsJson;
    int viewCount = 0, accCount = 0;

    auto addView = [&](size_t byteOffset, size_t byteLength) {
        if (viewCount) viewsJson += ",";
        viewsJson += "{\"buffer\":0,\"byteOffset\":" + std::to_string(byteOffset) +
                     ",\"byteLength\":" + std::to_string(byteLength) + "}";
        return viewCount++;
    };
    auto addAccessor = [&](const float *data, size_t comps, size_t count, const char *type, bool withMinMax) -> int {
        size_t byteOffset = bin.size() * sizeof(float);
        bin.insert(bin.end(), data, data + comps * count);
        int view = addView(byteOffset, comps * count * sizeof(float));
        if (accCount) accessorsJson += ",";
        accessorsJson += "{\"bufferView\":" + std::to_string(view) +
                         ",\"componentType\":5126,\"count\":" + std::to_string(count) +
                         ",\"type\":\"" + type + "\"";
        if (withMinMax) {
            float mn[3] = {data[0], data[1], data[2]}, mx[3] = {data[0], data[1], data[2]};
            for (size_t i = 1; i < count; ++i) {
                for (int c = 0; c < 3; ++c) {
                    mn[c] = mn[c] < data[i * comps + c] ? mn[c] : data[i * comps + c];
                    mx[c] = mx[c] > data[i * comps + c] ? mx[c] : data[i * comps + c];
                }
            }
            accessorsJson += ",\"min\":[" + fnum(mn[0]) + "," + fnum(mn[1]) + "," + fnum(mn[2]) + "]";
            accessorsJson += ",\"max\":[" + fnum(mx[0]) + "," + fnum(mx[1]) + "," + fnum(mx[2]) + "]";
        }
        accessorsJson += "}";
        return accCount++;
    };
    auto vec3Diff = [](const std::vector<v3> &target, const std::vector<v3> &base) {
        std::vector<float> diff(target.size() * 3);
        for (size_t i = 0; i < target.size(); ++i) {
            diff[i * 3 + 0] = target[i].x - base[i].x;
            diff[i * 3 + 1] = target[i].y - base[i].y;
            diff[i * 3 + 2] = target[i].z - base[i].z;
        }
        return diff;
    };

    const size_t n = spec.positions.size();
    std::string attrs = "\"POSITION\":" + std::to_string(addAccessor(&spec.positions[0].x, 3, n, "VEC3", true));
    if (!spec.normals.empty()) {
        attrs += ",\"NORMAL\":" + std::to_string(addAccessor(&spec.normals[0].x, 3, n, "VEC3", false));
    }
    if (!spec.tangents.empty()) {
        attrs += ",\"TANGENT\":" + std::to_string(addAccessor(&spec.tangents[0][0], 4, n, "VEC4", false));
    }
    if (!spec.uvs.empty()) {
        attrs += ",\"TEXCOORD_0\":" + std::to_string(addAccessor(&spec.uvs[0][0], 2, n, "VEC2", false));
    }

    std::string targetsJson, weightsJson, namesJson;
    for (size_t i = 0; i < spec.targets.size(); ++i) {
        const TargetSpec &t = spec.targets[i];
        if (i) {
            targetsJson += ",";
            weightsJson += ",";
            namesJson += ",";
        }
        std::string entry = "{";
        bool first = true;
        if (!t.positions.empty()) {
            std::vector<float> diff = vec3Diff(t.positions, spec.positions);
            entry += "\"POSITION\":" + std::to_string(addAccessor(diff.data(), 3, n, "VEC3", true));
            first = false;
        }
        if (!t.normals.empty()) {
            std::vector<float> diff = vec3Diff(t.normals, spec.normals);
            entry += std::string(first ? "" : ",") + "\"NORMAL\":" + std::to_string(addAccessor(diff.data(), 3, n, "VEC3", false));
            first = false;
        }
        if (!t.tangents.empty()) {
            std::vector<v3> baseT(n);
            for (size_t k = 0; k < n; ++k) baseT[k] = {spec.tangents[k][0], spec.tangents[k][1], spec.tangents[k][2]};
            std::vector<float> diff = vec3Diff(t.tangents, baseT);
            entry += std::string(first ? "" : ",") + "\"TANGENT\":" + std::to_string(addAccessor(diff.data(), 3, n, "VEC3", false));
            first = false;
        }
        entry += "}";
        targetsJson += entry;
        weightsJson += fnum(t.weight);
        namesJson += "\"" + t.name + "\"";
    }

    std::string json = "{";
    json += "\"asset\":{\"version\":\"2.0\"},";
    json += "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size() * sizeof(float)) +
            ",\"uri\":\"data:application/octet-stream;base64," + base64Encode(bin) + "\"}],";
    json += "\"bufferViews\":[" + viewsJson + "],";
    json += "\"accessors\":[" + accessorsJson + "],";
    json += "\"meshes\":[{\"name\":\"morph\",\"primitives\":[{\"attributes\":{" + attrs +
            "},\"mode\":4,\"targets\":[" + targetsJson + "]}],\"weights\":[" + weightsJson +
            "],\"extras\":{\"targetNames\":[" + namesJson + "]}}],";
    json += "\"nodes\":[{\"mesh\":0}],";
    json += "\"scenes\":[{\"nodes\":[0]}],";
    json += "\"scene\":0}";
    return json;
}

// ---------------------------------------------------------------------------
// Shared scene fixtures.
// ---------------------------------------------------------------------------
// Inclined triangle (not axis aligned), UVs form a right-angled grid.
static MeshSpec makeInclinedTriangle() {
    MeshSpec s;
    s.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 1.0f}};
    s.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    TargetSpec smile;
    smile.name = "smile";
    smile.weight = 0.25f;
    smile.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.5f}, {0.0f, 1.0f, 1.0f}};
    TargetSpec frown;
    frown.name = "frown";
    frown.weight = 0.75f;
    frown.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 2.0f}};
    s.targets = {smile, frown};
    return s;
}

// Same triangle, but the base mesh carries artist normals/tangents and the
// targets exercise every combination of present/missing streams.
static MeshSpec makeArtistScene() {
    MeshSpec s = makeInclinedTriangle();
    const float inv2 = 0.70710678118654752f; // 1/sqrt(2)
    s.normals = {{0.0f, -inv2, inv2}, {0.0f, -inv2, inv2}, {0.0f, -inv2, inv2}};
    s.tangents = {{1.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}};

    TargetSpec calm; // positions only -> both frames must be completed
    calm.name = "calm";
    calm.weight = 0.1f;
    calm.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.5f}, {0.0f, 1.0f, 1.0f}};

    TargetSpec artistN; // artist normals only -> tangents completed from them
    artistN.name = "artistn";
    artistN.weight = 0.2f;
    artistN.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.5f}, {0.0f, 1.0f, 1.0f}};
    artistN.normals = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}};

    TargetSpec artistT; // artist normals + tangents -> nothing is rebuilt
    artistT.name = "artistt";
    artistT.weight = 0.9f;
    artistT.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.5f}, {0.0f, 1.0f, 1.0f}};
    artistT.normals = {{1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    artistT.tangents = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}};

    s.targets = {calm, artistN, artistT};
    return s;
}

// Two triangles in verbose form; the targets place shared vertices
// differently so the spatial neighborhood differs per target.
static MeshSpec makeNeighborhoodScene() {
    MeshSpec s;
    s.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, // tri A
                   {2.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 1.0f}}; // tri B
    s.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f},
             {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    TargetSpec merge; // vertex 1 of tri A and vertex 0 of tri B coincide
    merge.name = "merge";
    merge.weight = 0.3f;
    merge.positions = {{0.0f, 0.0f, 0.0f}, {1.5f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                       {1.5f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 1.0f}};
    TargetSpec split; // same vertices stay apart (and one triangle tilts)
    split.name = "split";
    split.weight = 0.7f;
    split.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.3f}, {0.0f, 1.0f, 0.0f},
                       {2.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 1.0f}};
    s.targets = {merge, split};
    return s;
}

static const aiScene *importScene(Assimp::Importer &imp, const MeshSpec &spec, bool completion) {
    imp.SetPropertyBool(AI_CONFIG_PP_MORPH_FRAME_COMPLETION, completion);
    std::string json = buildGltf(spec);
    return imp.ReadFileFromMemory(json.data(), json.size(), 0, "gltf");
}

static const unsigned int kMorphFlags = aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;

// Expected smooth normal at one vertex: average the face normals of all
// vertices sharing the vertex' own position (default 175 degree rule).
static v3 expectedSmoothedNormal(const std::vector<v3> &pos, const std::vector<v3> &faceNrm, size_t idx, float eps) {
    v3 sum{0.0f, 0.0f, 0.0f};
    for (size_t j = 0; j < pos.size(); ++j) {
        v3 d = sub3(pos[j], pos[idx]);
        if (len3(d) <= eps) sum = add3(sum, faceNrm[j]);
    }
    return norm3(sum);
}
static float positionEpsilon(const std::vector<v3> &pos) {
    v3 mn = pos[0], mx = pos[0];
    for (size_t i = 1; i < pos.size(); ++i) {
        mn.x = mn.x < pos[i].x ? mn.x : pos[i].x;
        mn.y = mn.y < pos[i].y ? mn.y : pos[i].y;
        mn.z = mn.z < pos[i].z ? mn.z : pos[i].z;
        mx.x = mx.x > pos[i].x ? mx.x : pos[i].x;
        mx.y = mx.y > pos[i].y ? mx.y : pos[i].y;
        mx.z = mx.z > pos[i].z ? mx.z : pos[i].z;
    }
    return len3(sub3(mx, mn)) * 1e-4f;
}

// ---------------------------------------------------------------------------
// generation: frames are generated for the inclined triangle, direction
// checked independently against first-principles math.
// ---------------------------------------------------------------------------
static int gen_inclined_triangle_frames() {
    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, makeInclinedTriangle(), true);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    TASK_CHECK(scene->mNumMeshes == 1);
    const aiMesh *m = scene->mMeshes[0];
    TASK_CHECK(m->mNumAnimMeshes == 2);
    TASK_CHECK(m->HasNormals() && m->HasTangentsAndBitangents());

    float uv[3][2];
    for (int i = 0; i < 3; ++i) {
        uv[i][0] = m->mTextureCoords[0][i].x;
        uv[i][1] = m->mTextureCoords[0][i].y;
    }

    // Base frame, generated by the regular base steps.
    TriFrame base = expectedTriFrame(toV3(m->mVertices[0]), toV3(m->mVertices[1]), toV3(m->mVertices[2]), uv);
    for (unsigned int i = 0; i < 3; ++i) {
        TASK_CHECK(near3(m->mNormals[i], base.n));
        TASK_CHECK(near3(m->mTangents[i], base.t));
        TASK_CHECK(near3(m->mBitangents[i], base.b));
    }

    // Independent direction anchor for the inclined triangle: the geometric
    // normal of the "smile" endpoint is normalize(cross(p1-p0, p2-p0)).
    const aiAnimMesh *smile = m->mAnimMeshes[0];
    const aiAnimMesh *frown = m->mAnimMeshes[1];
    TASK_CHECK(smile->HasNormals() && smile->HasTangentsAndBitangents());
    TASK_CHECK(frown->HasNormals() && frown->HasTangentsAndBitangents());
    v3 smileN = norm3(cross3(sub3(toV3(smile->mVertices[1]), toV3(smile->mVertices[0])),
                             sub3(toV3(smile->mVertices[2]), toV3(smile->mVertices[0]))));
    TASK_CHECK(near1(smileN.x, -1.0f / 3.0f) && near1(smileN.y, -2.0f / 3.0f) && near1(smileN.z, 2.0f / 3.0f));

    TriFrame smileF = expectedTriFrame(toV3(smile->mVertices[0]), toV3(smile->mVertices[1]), toV3(smile->mVertices[2]), uv);
    TriFrame frownF = expectedTriFrame(toV3(frown->mVertices[0]), toV3(frown->mVertices[1]), toV3(frown->mVertices[2]), uv);
    for (unsigned int i = 0; i < 3; ++i) {
        TASK_CHECK(near3(smile->mNormals[i], smileF.n));
        TASK_CHECK(near3(smile->mTangents[i], smileF.t));
        TASK_CHECK(near3(smile->mBitangents[i], smileF.b));
        TASK_CHECK(near3(frown->mNormals[i], frownF.n));
        TASK_CHECK(near3(frown->mTangents[i], frownF.t));
        TASK_CHECK(near3(frown->mBitangents[i], frownF.b));
    }

    // Base and target endpoints must agree on tangent handedness.
    float hwBase = handedness(base.n, base.t, base.b);
    TASK_CHECK(handedness(smileF.n, smileF.t, smileF.b) == hwBase);
    TASK_CHECK(handedness(frownF.n, frownF.t, frownF.b) == hwBase);
    TASK_CHECK(handedness(toV3(smile->mNormals[0]), toV3(smile->mTangents[0]), toV3(smile->mBitangents[0])) == hwBase);
    return 0;
}

// ---------------------------------------------------------------------------
// generation: base mesh already has artist frames; targets are still checked.
// ---------------------------------------------------------------------------
static int gen_base_streams_present_targets_completed() {
    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, makeArtistScene(), true);
    TASK_CHECK(scene != nullptr);
    const aiMesh *m = scene->mMeshes[0];
    const aiVector3D *baseN = m->mNormals;
    const aiVector3D *baseT = m->mTangents;
    const aiVector3D *baseB = m->mBitangents;
    TASK_CHECK(baseN && baseT && baseB);
    v3 baseN0 = toV3(baseN[0]);

    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    m = scene->mMeshes[0];

    // Base streams are neither rebuilt nor modified.
    TASK_CHECK(m->mNormals == baseN && m->mTangents == baseT && m->mBitangents == baseB);
    TASK_CHECK(near3(m->mNormals[0], baseN0));
    TASK_CHECK(near3(m->mTangents[0], v3{1.0f, 0.0f, 0.0f}));

    // The positions-only target is completed even though the base streams
    // were already there.
    const aiAnimMesh *calm = m->mAnimMeshes[0];
    TASK_CHECK(calm->HasNormals() && calm->HasTangentsAndBitangents());
    float uv[3][2];
    for (int i = 0; i < 3; ++i) {
        uv[i][0] = m->mTextureCoords[0][i].x;
        uv[i][1] = m->mTextureCoords[0][i].y;
    }
    TriFrame calmF = expectedTriFrame(toV3(calm->mVertices[0]), toV3(calm->mVertices[1]), toV3(calm->mVertices[2]), uv);
    for (unsigned int i = 0; i < 3; ++i) {
        TASK_CHECK(near3(calm->mNormals[i], calmF.n));
        TASK_CHECK(near3(calm->mTangents[i], calmF.t));
        TASK_CHECK(near3(calm->mBitangents[i], calmF.b));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// generation: a target missing one stream class keeps its artist data and
// only gets the missing class completed.
// ---------------------------------------------------------------------------
static int gen_missing_single_stream() {
    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, makeArtistScene(), true);
    TASK_CHECK(scene != nullptr);
    const aiMesh *m = scene->mMeshes[0];
    const aiVector3D *artistNormals = m->mAnimMeshes[1]->mNormals;
    const aiVector3D *artistTangents = m->mAnimMeshes[2]->mTangents;
    const aiVector3D *artistBitangents = m->mAnimMeshes[2]->mBitangents;
    TASK_CHECK(artistNormals && artistTangents && artistBitangents);

    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    m = scene->mMeshes[0];

    // Artist normals are kept as-is; only the missing tangent frame appears.
    const aiAnimMesh *artistN = m->mAnimMeshes[1];
    TASK_CHECK(artistN->mNormals == artistNormals);
    TASK_CHECK(near3(artistN->mNormals[0], v3{0.0f, 0.0f, 1.0f}));
    TASK_CHECK(artistN->HasTangentsAndBitangents());
    // The completed tangents are derived from the target's own (artist)
    // normals, not from the base frame: with N=(0,0,1) the projected UV
    // axes give T=(1,0,0) and B=(0,1,0) exactly.
    for (unsigned int i = 0; i < 3; ++i) {
        TASK_CHECK(near3(artistN->mTangents[i], v3{1.0f, 0.0f, 0.0f}));
        TASK_CHECK(near3(artistN->mBitangents[i], v3{0.0f, 1.0f, 0.0f}));
    }

    // A target that already has both stream classes is left fully untouched.
    const aiAnimMesh *artistT = m->mAnimMeshes[2];
    TASK_CHECK(artistT->mTangents == artistTangents && artistT->mBitangents == artistBitangents);
    TASK_CHECK(near3(artistT->mNormals[0], v3{1.0f, 0.0f, 0.0f}));
    TASK_CHECK(near3(artistT->mTangents[0], v3{0.0f, 0.0f, 1.0f}));
    TASK_CHECK(near3(artistT->mBitangents[0], v3{0.0f, -1.0f, 0.0f}));
    return 0;
}

// ---------------------------------------------------------------------------
// generation: each stream class is governed by its own aiProcess flag; the
// tangent pass inherits base normals when the target has none.
// ---------------------------------------------------------------------------
static int gen_stream_flags_and_normal_inheritance() {
    // Tangents only: the normal pass is not requested, so the positions-only
    // target keeps no normals and its tangents are computed against the
    // inherited base normals.
    Assimp::Importer imp;
    MeshSpec spec = makeArtistScene();
    spec.targets.resize(1); // keep the positions-only "calm" target
    const aiScene *scene = importScene(imp, spec, true);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(aiProcess_CalcTangentSpace);
    TASK_CHECK(scene != nullptr);
    const aiMesh *m = scene->mMeshes[0];
    const aiAnimMesh *calm = m->mAnimMeshes[0];
    TASK_CHECK(calm->mNormals == nullptr);
    TASK_CHECK(calm->HasTangentsAndBitangents());

    // Expected frame: target positions + base artist normals + base UV0.
    float uv[3][2];
    for (int i = 0; i < 3; ++i) {
        uv[i][0] = m->mTextureCoords[0][i].x;
        uv[i][1] = m->mTextureCoords[0][i].y;
    }
    v3 p0 = toV3(calm->mVertices[0]), p1 = toV3(calm->mVertices[1]), p2 = toV3(calm->mVertices[2]);
    v3 v = sub3(p1, p0), w = sub3(p2, p0);
    float sx = uv[1][0] - uv[0][0], sy = uv[1][1] - uv[0][1];
    float tx = uv[2][0] - uv[0][0], ty = uv[2][1] - uv[0][1];
    float dc = (tx * sy - ty * sx) < 0.0f ? -1.0f : 1.0f;
    v3 tf{(w.x * sy - v.x * ty) * dc, (w.y * sy - v.y * ty) * dc, (w.z * sy - v.z * ty) * dc};
    v3 bf{(w.x * sx - v.x * tx) * dc, (w.y * sx - v.y * tx) * dc, (w.z * sx - v.z * tx) * dc};
    v3 baseN = toV3(m->mNormals[0]);
    v3 expT = norm3(sub3(tf, mul3(baseN, dot3(tf, baseN))));
    v3 expB = norm3(sub3(bf, mul3(baseN, dot3(bf, baseN))));
    for (unsigned int i = 0; i < 3; ++i) {
        TASK_CHECK(near3(calm->mTangents[i], expT));
        TASK_CHECK(near3(calm->mBitangents[i], expB));
    }

    // Normals only: the tangent pass is not requested, so no tangent frame
    // appears on the target.
    Assimp::Importer imp2;
    const aiScene *scene2 = importScene(imp2, makeInclinedTriangle(), true);
    TASK_CHECK(scene2 != nullptr);
    scene2 = imp2.ApplyPostProcessing(aiProcess_GenSmoothNormals);
    TASK_CHECK(scene2 != nullptr);
    const aiMesh *m2 = scene2->mMeshes[0];
    TASK_CHECK(m2->HasNormals() && !m2->HasTangentsAndBitangents());
    const aiAnimMesh *smile = m2->mAnimMeshes[0];
    TASK_CHECK(smile->HasNormals());
    TASK_CHECK(smile->mTangents == nullptr && smile->mBitangents == nullptr);
    TASK_CHECK(near3(smile->mNormals[0], v3{-1.0f / 3.0f, -2.0f / 3.0f, 2.0f / 3.0f}));
    return 0;
}

// ---------------------------------------------------------------------------
// generation: two targets whose position neighborhoods differ; smoothing
// follows each target's own positions, not the base pose grouping.
// ---------------------------------------------------------------------------
static int gen_neighborhoods_follow_target_positions() {
    Assimp::Importer imp;
    MeshSpec spec = makeNeighborhoodScene();
    const aiScene *scene = importScene(imp, spec, true);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    const aiMesh *m = scene->mMeshes[0];
    TASK_CHECK(m->mNumAnimMeshes == 2);

    const aiAnimMesh *merge = m->mAnimMeshes[0];
    const aiAnimMesh *split = m->mAnimMeshes[1];
    TASK_CHECK(merge->HasNormals() && split->HasNormals());
    TASK_CHECK(merge->HasTangentsAndBitangents() && split->HasTangentsAndBitangents());

    // Reference smoothing per target from its own positions.
    std::vector<v3> mergeFaceNrm(6), splitFaceNrm(6);
    for (int t = 0; t < 2; ++t) {
        const TargetSpec &ts = spec.targets[t];
        std::vector<v3> &out = t == 0 ? mergeFaceNrm : splitFaceNrm;
        for (int f = 0; f < 2; ++f) {
            v3 n = norm3(cross3(sub3(ts.positions[f * 3 + 1], ts.positions[f * 3 + 0]),
                                sub3(ts.positions[f * 3 + 2], ts.positions[f * 3 + 0])));
            out[f * 3 + 0] = out[f * 3 + 1] = out[f * 3 + 2] = n;
        }
    }
    float epsMerge = positionEpsilon(spec.targets[0].positions);
    float epsSplit = positionEpsilon(spec.targets[1].positions);
    for (unsigned int i = 0; i < 6; ++i) {
        TASK_CHECK(near3(merge->mNormals[i], expectedSmoothedNormal(spec.targets[0].positions, mergeFaceNrm, i, epsMerge)));
        TASK_CHECK(near3(split->mNormals[i], expectedSmoothedNormal(spec.targets[1].positions, splitFaceNrm, i, epsSplit)));
    }

    // In the "merge" target the two coincident vertices share one smoothed
    // normal; in the "split" target the same vertices keep their own.
    TASK_CHECK(near3(merge->mNormals[1], toV3(merge->mNormals[3]), 1e-6f));
    v3 merged = norm3(add3(v3{0.0f, 0.0f, 1.0f}, norm3(v3{0.0f, -1.0f, 1.0f})));
    TASK_CHECK(near3(merge->mNormals[1], merged));
    TASK_CHECK(!near3(split->mNormals[1], toV3(split->mNormals[3]), 1e-3f));
    TASK_CHECK(near3(split->mNormals[1], norm3(v3{-0.3f, 0.0f, 1.0f})));
    TASK_CHECK(near3(split->mNormals[3], norm3(v3{0.0f, -1.0f, 1.0f})));
    return 0;
}

// ---------------------------------------------------------------------------
// generation: a target without own positions is completed against the base
// positions, so its frames come out equal to the base frames.
// ---------------------------------------------------------------------------
static int gen_target_without_positions_uses_base() {
    MeshSpec spec = makeInclinedTriangle();
    TargetSpec still; // no positions, no normals, no tangents
    still.name = "still";
    still.weight = 0.5f;
    spec.targets.clear();
    spec.targets.push_back(still);

    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, spec, true);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    const aiMesh *m = scene->mMeshes[0];
    TASK_CHECK(m->mNumAnimMeshes == 1);
    const aiAnimMesh *am = m->mAnimMeshes[0];
    TASK_CHECK(!am->HasPositions());
    TASK_CHECK(am->HasNormals() && am->HasTangentsAndBitangents());
    TASK_CHECK(am->mNormals != m->mNormals && am->mTangents != m->mTangents);
    for (unsigned int i = 0; i < 3; ++i) {
        TASK_CHECK(near3(am->mNormals[i], toV3(m->mNormals[i]), 1e-6f));
        TASK_CHECK(near3(am->mTangents[i], toV3(m->mTangents[i]), 1e-6f));
        TASK_CHECK(near3(am->mBitangents[i], toV3(m->mBitangents[i]), 1e-6f));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// generation: running the post-processing chain twice does not rebuild
// frames that already exist.
// ---------------------------------------------------------------------------
static int gen_repeated_processing_keeps_frames() {
    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, makeInclinedTriangle(), true);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    const aiMesh *m = scene->mMeshes[0];
    const aiVector3D *baseN = m->mNormals;
    const aiVector3D *baseT = m->mTangents;
    const aiVector3D *t0n = m->mAnimMeshes[0]->mNormals;
    const aiVector3D *t0t = m->mAnimMeshes[0]->mTangents;
    const aiVector3D *t1n = m->mAnimMeshes[1]->mNormals;
    const aiVector3D *t1b = m->mAnimMeshes[1]->mBitangents;
    TASK_CHECK(baseN && baseT && t0n && t0t && t1n && t1b);
    v3 keep0 = toV3(t0n[0]), keep1 = toV3(t1b[2]), keepBase = toV3(baseT[1]);

    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    m = scene->mMeshes[0];
    TASK_CHECK(m->mNormals == baseN && m->mTangents == baseT);
    TASK_CHECK(m->mAnimMeshes[0]->mNormals == t0n && m->mAnimMeshes[0]->mTangents == t0t);
    TASK_CHECK(m->mAnimMeshes[1]->mNormals == t1n && m->mAnimMeshes[1]->mBitangents == t1b);
    TASK_CHECK(near3(m->mAnimMeshes[0]->mNormals[0], keep0, 1e-6f));
    TASK_CHECK(near3(m->mAnimMeshes[1]->mBitangents[2], keep1, 1e-6f));
    TASK_CHECK(near3(m->mTangents[1], keepBase, 1e-6f));
    return 0;
}

// ---------------------------------------------------------------------------
// generation: completion is opt-in; with the property unset the legacy
// behavior (positions-only targets) is preserved.
// ---------------------------------------------------------------------------
static int gen_completion_is_opt_in() {
    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, makeInclinedTriangle(), false);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    const aiMesh *m = scene->mMeshes[0];
    TASK_CHECK(m->HasNormals() && m->HasTangentsAndBitangents());
    for (unsigned int a = 0; a < m->mNumAnimMeshes; ++a) {
        const aiAnimMesh *am = m->mAnimMeshes[a];
        TASK_CHECK(am->HasPositions());
        TASK_CHECK(am->mNormals == nullptr && am->mTangents == nullptr && am->mBitangents == nullptr);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ownership: completed frames are held per target; base and sibling streams
// are independent and scene structure is untouched.
// ---------------------------------------------------------------------------
static int own_completed_frames_are_independent() {
    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, makeInclinedTriangle(), true);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    const aiMesh *m = scene->mMeshes[0];
    aiAnimMesh *t0 = m->mAnimMeshes[0];
    aiAnimMesh *t1 = m->mAnimMeshes[1];

    // Distinct arrays everywhere.
    TASK_CHECK(t0->mNormals != t1->mNormals);
    TASK_CHECK(t0->mNormals != m->mNormals && t1->mNormals != m->mNormals);
    TASK_CHECK(t0->mTangents != t1->mTangents);
    TASK_CHECK(t0->mTangents != m->mTangents && t1->mTangents != m->mTangents);
    TASK_CHECK(t0->mBitangents != t1->mBitangents);
    TASK_CHECK(t0->mBitangents != m->mBitangents && t1->mBitangents != m->mBitangents);

    // Mutating one target leaves the base mesh and the sibling untouched.
    v3 baseKeep = toV3(m->mNormals[0]);
    v3 siblingKeep = toV3(t1->mNormals[0]);
    t0->mNormals[0] = aiVector3D(9.0f, 9.0f, 9.0f);
    TASK_CHECK(near3(m->mNormals[0], baseKeep, 1e-6f));
    TASK_CHECK(near3(t1->mNormals[0], siblingKeep, 1e-6f));

    // Vertex, face and target order plus names and weights are unchanged.
    TASK_CHECK(m->mNumVertices == 3 && m->mNumFaces == 1);
    TASK_CHECK(m->mFaces[0].mNumIndices == 3);
    TASK_CHECK(m->mFaces[0].mIndices[0] == 0 && m->mFaces[0].mIndices[1] == 1 && m->mFaces[0].mIndices[2] == 2);
    TASK_CHECK(m->mNumAnimMeshes == 2);
    TASK_CHECK(std::strcmp(t0->mName.C_Str(), "smile") == 0);
    TASK_CHECK(std::strcmp(t1->mName.C_Str(), "frown") == 0);
    TASK_CHECK(near1(t0->mWeight, 0.25f, 1e-6f));
    TASK_CHECK(near1(t1->mWeight, 0.75f, 1e-6f));
    return 0;
}

// ---------------------------------------------------------------------------
// ownership: destroying one target releases exactly its own arrays; the base
// streams and the sibling target stay valid.
// ---------------------------------------------------------------------------
static int own_destroying_target_keeps_base_and_siblings() {
    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, makeInclinedTriangle(), true);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    aiMesh *m = scene->mMeshes[0];
    TASK_CHECK(m->mNumAnimMeshes == 2);

    aiAnimMesh *victim = m->mAnimMeshes[0];
    aiAnimMesh *sibling = m->mAnimMeshes[1];
    const aiVector3D *victimV = victim->mVertices;
    const aiVector3D *victimN = victim->mNormals;
    const aiVector3D *victimT = victim->mTangents;
    const aiVector3D *victimB = victim->mBitangents;
    const aiVector3D *siblingN = sibling->mNormals;
    const aiVector3D *siblingT = sibling->mTangents;
    const aiVector3D *siblingB = sibling->mBitangents;
    const aiVector3D *siblingV = sibling->mVertices;
    v3 siblingKeep = toV3(siblingN[0]);
    v3 baseKeepN = toV3(m->mNormals[0]);
    v3 baseKeepT = toV3(m->mTangents[0]);

    freedPointers().clear();
    g_recordFrees = true;
    delete victim;
    g_recordFrees = false;

    // Exactly the victim's own arrays were released ...
    TASK_CHECK(freedPointers().count(const_cast<aiVector3D *>(victimV)) == 1);
    TASK_CHECK(freedPointers().count(const_cast<aiVector3D *>(victimN)) == 1);
    TASK_CHECK(freedPointers().count(const_cast<aiVector3D *>(victimT)) == 1);
    TASK_CHECK(freedPointers().count(const_cast<aiVector3D *>(victimB)) == 1);
    // ... and nothing belonging to the base mesh or the sibling was freed.
    TASK_CHECK(freedPointers().count(m->mVertices) == 0);
    TASK_CHECK(freedPointers().count(m->mNormals) == 0);
    TASK_CHECK(freedPointers().count(m->mTangents) == 0);
    TASK_CHECK(freedPointers().count(m->mBitangents) == 0);
    TASK_CHECK(freedPointers().count(const_cast<aiVector3D *>(siblingV)) == 0);
    TASK_CHECK(freedPointers().count(const_cast<aiVector3D *>(siblingN)) == 0);
    TASK_CHECK(freedPointers().count(const_cast<aiVector3D *>(siblingT)) == 0);
    TASK_CHECK(freedPointers().count(const_cast<aiVector3D *>(siblingB)) == 0);

    // Detach the destroyed target so the scene teardown stays consistent.
    m->mAnimMeshes[0] = m->mAnimMeshes[1];
    m->mAnimMeshes[1] = nullptr;
    m->mNumAnimMeshes = 1;

    // Base and sibling frames are still intact.
    TASK_CHECK(near3(m->mNormals[0], baseKeepN, 1e-6f));
    TASK_CHECK(near3(m->mTangents[0], baseKeepT, 1e-6f));
    TASK_CHECK(sibling->mNormals == siblingN && sibling->mTangents == siblingT);
    TASK_CHECK(near3(sibling->mNormals[0], siblingKeep, 1e-6f));
    return 0;
}

// ---------------------------------------------------------------------------
// ownership: scene teardown releases the completed frames together with
// their owning targets (no leak, no premature free).
// ---------------------------------------------------------------------------
static int own_scene_teardown_releases_completed_frames() {
    std::vector<const aiVector3D *> watched;
    {
        Assimp::Importer imp;
        const aiScene *scene = importScene(imp, makeInclinedTriangle(), true);
        TASK_CHECK(scene != nullptr);
        scene = imp.ApplyPostProcessing(kMorphFlags);
        TASK_CHECK(scene != nullptr);
        const aiMesh *m = scene->mMeshes[0];
        watched.push_back(m->mNormals);
        watched.push_back(m->mTangents);
        for (unsigned int a = 0; a < m->mNumAnimMeshes; ++a) {
            const aiAnimMesh *am = m->mAnimMeshes[a];
            watched.push_back(am->mVertices);
            watched.push_back(am->mNormals);
            watched.push_back(am->mTangents);
            watched.push_back(am->mBitangents);
        }
        freedPointers().clear();
        g_recordFrees = true;
    } // importer and scene are destroyed here
    g_recordFrees = false;

    TASK_CHECK(watched.size() == 10);
    for (size_t i = 0; i < watched.size(); ++i) {
        TASK_CHECK(freedPointers().count(const_cast<aiVector3D *>(watched[i])) == 1);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// glTF helpers: read back a file, parse JSON, extract float accessor data.
// ---------------------------------------------------------------------------
static bool readWholeFile(const std::string &path, std::string &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    size_t got = size ? std::fread(&out[0], 1, static_cast<size_t>(size), f) : 0;
    std::fclose(f);
    return got == static_cast<size_t>(size);
}

struct AccessorData {
    int componentType = 0;
    std::string type;
    size_t count = 0;
    std::vector<float> values;
};
static bool readFloatAccessor(const rapidjson::Document &doc, const std::string &bin, int index, AccessorData &out) {
    if (!doc.HasMember("accessors")) return false;
    const rapidjson::Value &accs = doc["accessors"];
    if (index < 0 || static_cast<rapidjson::SizeType>(index) >= accs.Size()) return false;
    const rapidjson::Value &acc = accs[index];
    out.componentType = acc["componentType"].GetInt();
    out.type = acc["type"].GetString();
    out.count = acc["count"].GetUint64();
    if (out.componentType != 5126) return false; // FLOAT only here
    size_t offset = acc.HasMember("byteOffset") ? acc["byteOffset"].GetUint64() : 0;
    const rapidjson::Value &bv = doc["bufferViews"][acc["bufferView"].GetInt()];
    offset += bv.HasMember("byteOffset") ? bv["byteOffset"].GetUint64() : 0;
    int comps = out.type == "SCALAR" ? 1 : out.type == "VEC2" ? 2 : out.type == "VEC3" ? 3 : 4;
    out.values.resize(out.count * static_cast<size_t>(comps));
    if (offset + out.values.size() * sizeof(float) > bin.size()) return false;
    std::memcpy(out.values.data(), bin.data() + offset, out.values.size() * sizeof(float));
    return true;
}

static bool loadGltfAndBin(const std::string &gltfPath, rapidjson::Document &doc, std::string &bin) {
    std::string text;
    if (!readWholeFile(gltfPath, text)) return false;
    doc.Parse(text.c_str());
    if (doc.HasParseError()) return false;
    if (!doc.HasMember("buffers") || doc["buffers"].Size() == 0) return false;
    std::string uri = doc["buffers"][0]["uri"].GetString();
    std::string dir = gltfPath.substr(0, gltfPath.find_last_of("/\\") + 1);
    return readWholeFile(dir + uri, bin);
}

static const std::string kExportPath = ".task-build/morph_frames_exp.gltf";
static const std::string kLegacyPath = ".task-build/morph_frames_legacy.gltf";
static const std::string kNormalOnlyPath = ".task-build/morph_frames_normalonly.gltf";

// Captured endpoint frames of the inclined-triangle scene, taken before the
// exporter (which normalizes base streams in place) runs.
struct CapturedScene {
    std::vector<v3> basePos, baseN, baseT, baseB;
    std::vector<v3> t0pos, t0n, t0t;
    std::vector<v3> t1pos, t1n, t1t;
};
static void capture(const aiMesh *m, CapturedScene &c) {
    const aiAnimMesh *a0 = m->mAnimMeshes[0];
    const aiAnimMesh *a1 = m->mAnimMeshes[1];
    for (unsigned int i = 0; i < 3; ++i) {
        c.basePos.push_back(toV3(m->mVertices[i]));
        c.baseN.push_back(toV3(m->mNormals[i]));
        c.baseT.push_back(toV3(m->mTangents[i]));
        c.baseB.push_back(toV3(m->mBitangents[i]));
        c.t0pos.push_back(toV3(a0->mVertices[i]));
        c.t0n.push_back(toV3(a0->mNormals[i]));
        c.t0t.push_back(toV3(a0->mTangents[i]));
        c.t1pos.push_back(toV3(a1->mVertices[i]));
        c.t1n.push_back(toV3(a1->mNormals[i]));
        c.t1t.push_back(toV3(a1->mTangents[i]));
    }
}

// ---------------------------------------------------------------------------
// gltf: the export switches store per-target diffs as plain FLOAT accessors.
// ---------------------------------------------------------------------------
static int gltf_export_stores_target_deltas() {
    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, makeInclinedTriangle(), true);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    const aiMesh *m = scene->mMeshes[0];
    CapturedScene cap;
    capture(m, cap);

    Assimp::Exporter exporter;
    Assimp::ExportProperties props;
    props.SetPropertyBool("GLTF2_TARGET_TANGENT_EXP", true);
    props.SetPropertyBool("GLTF2_TARGET_NORMAL_EXP", true);
    props.SetPropertyBool("GLTF2_TARGETNAMES_EXP", true);
    TASK_CHECK(exporter.Export(scene, "gltf2", kExportPath, 0, &props) == aiReturn_SUCCESS);

    rapidjson::Document doc;
    std::string bin;
    TASK_CHECK(loadGltfAndBin(kExportPath, doc, bin));
    TASK_CHECK(doc.HasMember("meshes") && doc["meshes"].Size() == 1);
    const rapidjson::Value &mesh = doc["meshes"][0];
    TASK_CHECK(mesh.HasMember("extras") && mesh["extras"].HasMember("targetNames"));
    const rapidjson::Value &names = mesh["extras"]["targetNames"];
    TASK_CHECK(names.Size() == 2);
    TASK_CHECK(std::strcmp(names[0].GetString(), "smile") == 0);
    TASK_CHECK(std::strcmp(names[1].GetString(), "frown") == 0);

    const rapidjson::Value &targets = mesh["primitives"][0]["targets"];
    TASK_CHECK(targets.Size() == 2);
    for (int t = 0; t < 2; ++t) {
        const rapidjson::Value &target = targets[t];
        TASK_CHECK(target.HasMember("POSITION") && target.HasMember("NORMAL") && target.HasMember("TANGENT"));
        const std::vector<v3> &tp = t == 0 ? cap.t0pos : cap.t1pos;
        const std::vector<v3> &tn = t == 0 ? cap.t0n : cap.t1n;
        const std::vector<v3> &tt = t == 0 ? cap.t0t : cap.t1t;

        AccessorData pos, nrm, tan;
        TASK_CHECK(readFloatAccessor(doc, bin, target["POSITION"].GetInt(), pos));
        TASK_CHECK(readFloatAccessor(doc, bin, target["NORMAL"].GetInt(), nrm));
        TASK_CHECK(readFloatAccessor(doc, bin, target["TANGENT"].GetInt(), tan));
        // plain non-sparse FLOAT/VEC3 accessors
        TASK_CHECK(pos.type == "VEC3" && nrm.type == "VEC3" && tan.type == "VEC3");
        TASK_CHECK(pos.componentType == 5126 && nrm.componentType == 5126 && tan.componentType == 5126);
        TASK_CHECK(pos.count == 3 && nrm.count == 3 && tan.count == 3);
        for (size_t i = 0; i < 3; ++i) {
            // every stored frame is the endpoint minus the base frame
            TASK_CHECK(near1(pos.values[i * 3 + 0], tp[i].x - cap.basePos[i].x, 1e-5f));
            TASK_CHECK(near1(pos.values[i * 3 + 1], tp[i].y - cap.basePos[i].y, 1e-5f));
            TASK_CHECK(near1(pos.values[i * 3 + 2], tp[i].z - cap.basePos[i].z, 1e-5f));
            TASK_CHECK(near1(nrm.values[i * 3 + 0], tn[i].x - cap.baseN[i].x));
            TASK_CHECK(near1(nrm.values[i * 3 + 1], tn[i].y - cap.baseN[i].y));
            TASK_CHECK(near1(nrm.values[i * 3 + 2], tn[i].z - cap.baseN[i].z));
            TASK_CHECK(near1(tan.values[i * 3 + 0], tt[i].x - cap.baseT[i].x));
            TASK_CHECK(near1(tan.values[i * 3 + 1], tt[i].y - cap.baseT[i].y));
            TASK_CHECK(near1(tan.values[i * 3 + 2], tt[i].z - cap.baseT[i].z));
        }
    }

    // The base TANGENT accessor keeps its handedness in w.
    const rapidjson::Value &attrs = mesh["primitives"][0]["attributes"];
    TASK_CHECK(attrs.HasMember("TANGENT"));
    AccessorData baseTan;
    TASK_CHECK(readFloatAccessor(doc, bin, attrs["TANGENT"].GetInt(), baseTan));
    TASK_CHECK(baseTan.type == "VEC4" && baseTan.count == 3);
    for (size_t i = 0; i < 3; ++i) {
        TASK_CHECK(near1(baseTan.values[i * 4 + 0], cap.baseT[i].x));
        TASK_CHECK(near1(baseTan.values[i * 4 + 1], cap.baseT[i].y));
        TASK_CHECK(near1(baseTan.values[i * 4 + 2], cap.baseT[i].z));
        TASK_CHECK(near1(baseTan.values[i * 4 + 3], handedness(cap.baseN[i], cap.baseT[i], cap.baseB[i])));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// gltf: reimporting the exported file restores every endpoint frame; target
// bitangents follow the glTF representation w*(N x T), verified here with
// an independent hand-rolled computation.
// ---------------------------------------------------------------------------
static int gltf_reimport_restores_endpoint_frames() {
    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, makeInclinedTriangle(), true);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);
    CapturedScene cap;
    capture(scene->mMeshes[0], cap);

    Assimp::Exporter exporter;
    Assimp::ExportProperties props;
    props.SetPropertyBool("GLTF2_TARGET_TANGENT_EXP", true);
    props.SetPropertyBool("GLTF2_TARGET_NORMAL_EXP", true);
    props.SetPropertyBool("GLTF2_TARGETNAMES_EXP", true);
    TASK_CHECK(exporter.Export(scene, "gltf2", kExportPath, 0, &props) == aiReturn_SUCCESS);

    Assimp::Importer again;
    const aiScene *re = again.ReadFile(kExportPath, 0);
    TASK_CHECK(re != nullptr);
    TASK_CHECK(re->mNumMeshes == 1);
    const aiMesh *rm = re->mMeshes[0];
    TASK_CHECK(rm->HasNormals() && rm->HasTangentsAndBitangents());
    TASK_CHECK(rm->mNumAnimMeshes == 2);

    // Target order and names survive the round trip.
    TASK_CHECK(std::strcmp(rm->mAnimMeshes[0]->mName.C_Str(), "smile") == 0);
    TASK_CHECK(std::strcmp(rm->mAnimMeshes[1]->mName.C_Str(), "frown") == 0);

    // Base handedness as recovered from the original scene.
    float w[3];
    for (int i = 0; i < 3; ++i) {
        w[i] = handedness(cap.baseN[i], cap.baseT[i], cap.baseB[i]);
    }

    // Base endpoint: normals/tangents restored, bitangent = w*(N x T).
    for (unsigned int i = 0; i < 3; ++i) {
        TASK_CHECK(near3(rm->mNormals[i], cap.baseN[i]));
        TASK_CHECK(near3(rm->mTangents[i], cap.baseT[i]));
        v3 expB = mul3(norm3(cross3(cap.baseN[i], cap.baseT[i])), w[i]);
        TASK_CHECK(near3(rm->mBitangents[i], expB));
    }

    // Morph target endpoints: normals and tangents restored per target,
    // bitangents rebuilt as w*(N x T) with the base tangent's w.
    for (int t = 0; t < 2; ++t) {
        const aiAnimMesh *am = rm->mAnimMeshes[t];
        TASK_CHECK(am->HasNormals() && am->HasTangentsAndBitangents());
        const std::vector<v3> &tn = t == 0 ? cap.t0n : cap.t1n;
        const std::vector<v3> &tt = t == 0 ? cap.t0t : cap.t1t;
        for (unsigned int i = 0; i < 3; ++i) {
            TASK_CHECK(near3(am->mNormals[i], tn[i]));
            TASK_CHECK(near3(am->mTangents[i], tt[i]));
            v3 expB = mul3(norm3(cross3(tn[i], tt[i])), w[i]);
            TASK_CHECK(near3(am->mBitangents[i], expB));
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// gltf: with the legacy switches off the export keeps the old layout
// (positions only); the normal switch alone does not add tangents.
// ---------------------------------------------------------------------------
static int gltf_legacy_switches_keep_old_layout() {
    Assimp::Importer imp;
    const aiScene *scene = importScene(imp, makeInclinedTriangle(), true);
    TASK_CHECK(scene != nullptr);
    scene = imp.ApplyPostProcessing(kMorphFlags);
    TASK_CHECK(scene != nullptr);

    {
        Assimp::Exporter exporter; // no switches at all
        TASK_CHECK(exporter.Export(scene, "gltf2", kLegacyPath) == aiReturn_SUCCESS);
        rapidjson::Document doc;
        std::string bin;
        TASK_CHECK(loadGltfAndBin(kLegacyPath, doc, bin));
        const rapidjson::Value &targets = doc["meshes"][0]["primitives"][0]["targets"];
        TASK_CHECK(targets.Size() == 2);
        for (unsigned int t = 0; t < targets.Size(); ++t) {
            TASK_CHECK(targets[t].HasMember("POSITION"));
            TASK_CHECK(!targets[t].HasMember("NORMAL"));
            TASK_CHECK(!targets[t].HasMember("TANGENT"));
        }
    }
    {
        Assimp::Exporter exporter;
        Assimp::ExportProperties props;
        props.SetPropertyBool("GLTF2_TARGET_NORMAL_EXP", true);
        TASK_CHECK(exporter.Export(scene, "gltf2", kNormalOnlyPath, 0, &props) == aiReturn_SUCCESS);
        rapidjson::Document doc;
        std::string bin;
        TASK_CHECK(loadGltfAndBin(kNormalOnlyPath, doc, bin));
        const rapidjson::Value &targets = doc["meshes"][0]["primitives"][0]["targets"];
        TASK_CHECK(targets.Size() == 2);
        for (unsigned int t = 0; t < targets.Size(); ++t) {
            TASK_CHECK(targets[t].HasMember("POSITION"));
            TASK_CHECK(targets[t].HasMember("NORMAL"));
            TASK_CHECK(!targets[t].HasMember("TANGENT"));
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Case registry for the task driver.
// ---------------------------------------------------------------------------
const struct task_case task_cases[] = {
        {"generation", "inclined_triangle_frames", gen_inclined_triangle_frames},
        {"generation", "base_streams_present_targets_completed", gen_base_streams_present_targets_completed},
        {"generation", "missing_single_stream", gen_missing_single_stream},
        {"generation", "stream_flags_and_normal_inheritance", gen_stream_flags_and_normal_inheritance},
        {"generation", "neighborhoods_follow_target_positions", gen_neighborhoods_follow_target_positions},
        {"generation", "target_without_positions_uses_base", gen_target_without_positions_uses_base},
        {"generation", "repeated_processing_keeps_frames", gen_repeated_processing_keeps_frames},
        {"generation", "completion_is_opt_in", gen_completion_is_opt_in},
        {"ownership", "completed_frames_are_independent", own_completed_frames_are_independent},
        {"ownership", "destroying_target_keeps_base_and_siblings", own_destroying_target_keeps_base_and_siblings},
        {"ownership", "scene_teardown_releases_completed_frames", own_scene_teardown_releases_completed_frames},
        {"gltf", "export_stores_target_deltas", gltf_export_stores_target_deltas},
        {"gltf", "reimport_restores_endpoint_frames", gltf_reimport_restores_endpoint_frames},
        {"gltf", "legacy_switches_keep_old_layout", gltf_legacy_switches_keep_old_layout},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
