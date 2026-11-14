/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Morph target frame completion tests.

The cases exercise the optional AI_CONFIG_PP_MORPH_FRAME_COMPLETION
post-processing (missing normal/tangent streams are filled directly on
each aiAnimMesh) and the GLTF2_TARGET_TANGENT_EXP exporter switch
(morph target tangents are written as FLOAT/VEC3 differences).

All geometry is embedded in morph_frames_data.h as self-contained glTF
with data-URI buffers, so no model files are downloaded or read.
*/

#include "task_support/check.h"
#include "morph_frames_data.h"

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <rapidjson/document.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr unsigned int kBothFlags = aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;

// --- tiny independent vector math (deliberately not assimp operators) ---
struct V3 {
    float x, y, z;
};

V3 Cross(const V3 &a, const V3 &b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Dot(const V3 &a, const V3 &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

V3 Normalized(const V3 &a) {
    const float len = std::sqrt(Dot(a, a));
    return len > 0.0f ? V3{a.x / len, a.y / len, a.z / len} : a;
}

V3 Of(const aiVector3D &v) {
    return {v.x, v.y, v.z};
}

bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

bool NearV(const aiVector3D &got, const V3 &expected, float eps = 1e-4f) {
    return Near(got.x, expected.x, eps) && Near(got.y, expected.y, eps) && Near(got.z, expected.z, eps);
}

// --- scene loading helpers ---
struct Loaded {
    std::unique_ptr<Assimp::Importer> importer;
    const aiScene *scene = nullptr;
};

Loaded LoadGlTF(const char *json, bool completion, unsigned int flags) {
    Loaded loaded;
    loaded.importer.reset(new Assimp::Importer());
    if (completion) {
        loaded.importer->SetPropertyBool(AI_CONFIG_PP_MORPH_FRAME_COMPLETION, true);
    }
    loaded.scene = loaded.importer->ReadFileFromMemory(json, std::strlen(json), 0, "gltf");
    if (loaded.scene != nullptr && flags != 0) {
        loaded.scene = loaded.importer->ApplyPostProcessing(flags);
    }
    return loaded;
}

std::string WorkDir() {
    const char *td = std::getenv("TMPDIR");
    return td != nullptr ? std::string(td) : std::string("/tmp");
}

std::string ReadText(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<uint8_t> ReadBinary(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Decodes one FLOAT accessor from an exported .gltf + its .bin body.
// Reads through the JSON and raw bytes, i.e. independently of the assimp
// accessor classes used by the importer/exporter.
struct GltfFile {
    rapidjson::Document doc;
    std::vector<uint8_t> bin;
};

bool OpenGltfFile(const std::string &gltfPath, std::unique_ptr<GltfFile> &out) {
    auto file = std::unique_ptr<GltfFile>(new GltfFile());
    file->doc.Parse(ReadText(gltfPath).c_str());
    if (file->doc.HasParseError()) {
        return false;
    }
    const auto &buffer = file->doc["buffers"][0];
    std::string uri = buffer["uri"].GetString();
    const std::string binName = uri.substr(uri.find_last_of("/\\") + 1);
    const std::string dir = gltfPath.substr(0, gltfPath.find_last_of("/\\") + 1);
    file->bin = ReadBinary(dir + binName);
    out = std::move(file);
    return true;
}

bool DecodeAccessor(const GltfFile &file, unsigned int accessorIndex, int expectedComponents,
        std::vector<std::array<float, 4>> &out) {
    const auto &accessor = file.doc["accessors"][accessorIndex];
    if (!accessor.IsObject() || accessor["componentType"].GetInt() != 5126 /* FLOAT */ ||
            accessor.HasMember("sparse") /* plain accessor only */) {
        return false;
    }
    const char *type = accessor["type"].GetString();
    const int components = std::strcmp(type, "VEC3") == 0 ? 3 : (std::strcmp(type, "VEC4") == 0 ? 4 : 1);
    if (components != expectedComponents) {
        return false;
    }

    const auto &view = file.doc["bufferViews"][accessor["bufferView"].GetInt()];
    const size_t offset = view["byteOffset"].GetUint64() +
                          (accessor.HasMember("byteOffset") ? accessor["byteOffset"].GetUint64() : 0u);
    const size_t count = accessor["count"].GetUint64();

    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        std::array<float, 4> v{{0.0f, 0.0f, 0.0f, 0.0f}};
        std::memcpy(v.data(), file.bin.data() + offset + i * components * sizeof(float),
                components * sizeof(float));
        out[i] = v;
    }
    return true;
}

constexpr float kSqrt5 = 2.2360679775f;
constexpr float kSqrt2 = 1.41421356237f;
constexpr float kSqrt58 = 7.61577310586f;

} // namespace

// ===========================================================================
// generation group
// ===========================================================================

// Base pose already carries normals/tangents; with the option enabled every
// position-only target receives its own completed normal and tangent pair.
static int GenerationCompletesAllTargets() {
    Loaded loaded = LoadGlTF(kSceneNeighborhoods, true, kBothFlags);
    TASK_CHECK(loaded.scene != nullptr);
    const aiMesh *mesh = loaded.scene->mMeshes[0];
    TASK_CHECK(mesh->mNumVertices == 6);
    TASK_CHECK(mesh->mNumAnimMeshes == 2);
    TASK_CHECK(mesh->HasNormals());
    TASK_CHECK(mesh->HasTangentsAndBitangents());

    for (unsigned int t = 0; t < mesh->mNumAnimMeshes; ++t) {
        const aiAnimMesh *target = mesh->mAnimMeshes[t];
        TASK_CHECK(target->HasPositions());
        TASK_CHECK(target->HasNormals());
        TASK_CHECK(target->HasTangentsAndBitangents());
        TASK_CHECK(target->mBitangents != nullptr);
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            TASK_CHECK(std::isfinite(target->mNormals[v].x) && std::isfinite(target->mNormals[v].z));
            TASK_CHECK(std::isfinite(target->mTangents[v].x));
            TASK_CHECK(std::isfinite(target->mBitangents[v].y));
        }
    }
    return 0;
}

// The two targets produce different co-located neighborhoods: target 0 lifts
// v3 away from v0 (splitting the base group), target 1 merges v4 onto v1.
// A reused base-pose grouping could not reproduce both answers.
static int GenerationPerTargetNeighborhoods() {
    Loaded loaded = LoadGlTF(kSceneNeighborhoods, true, kBothFlags);
    TASK_CHECK(loaded.scene != nullptr);
    const aiMesh *mesh = loaded.scene->mMeshes[0];

    const aiAnimMesh *split = mesh->mAnimMeshes[0];
    const aiAnimMesh *merge = mesh->mAnimMeshes[1];

    // Split target: v3..v5 share the tilted face normal (1,1,2)/sqrt(6)
    // while v0..v2 keep the flat +z normal.
    const V3 tilted = Normalized({1.0f, 1.0f, 2.0f});
    TASK_CHECK(NearV(split->mNormals[0], {0.0f, 0.0f, 1.0f}));
    TASK_CHECK(NearV(split->mNormals[3], tilted));
    TASK_CHECK(NearV(split->mNormals[5], tilted));
    // Tangent tilts along -z for the lifted group.
    TASK_CHECK(NearV(split->mTangents[3], Normalized({2.0f, 0.0f, -1.0f})));
    TASK_CHECK(NearV(split->mBitangents[3], Normalized({0.0f, 2.0f, -1.0f})));

    // Merge target: every vertex stays flat, normals remain +z.
    for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
        TASK_CHECK(NearV(merge->mNormals[v], {0.0f, 0.0f, 1.0f}));
        TASK_CHECK(NearV(merge->mTangents[v], {1.0f, 0.0f, 0.0f}));
        TASK_CHECK(NearV(merge->mBitangents[v], {0.0f, 1.0f, 0.0f}));
    }

    // The two targets must disagree at v3, proving the neighborhood was
    // rebuilt per target rather than shared from the base pose.
    TASK_CHECK(!NearV(split->mNormals[3], Of(merge->mNormals[3]), 1e-3f));
    return 0;
}

// A single slanted triangle, checked against hand-derived values. The
// deformed target has normal (0,-3,7)/sqrt(58), tangent (1,0,0) and the
// bitangent must satisfy B = N x T independently of the library code path.
static int GenerationSlantedTriangleAnalytic() {
    Loaded loaded = LoadGlTF(kSceneSlanted, true, kBothFlags);
    TASK_CHECK(loaded.scene != nullptr);
    const aiMesh *mesh = loaded.scene->mMeshes[0];
    TASK_CHECK(mesh->mNumVertices == 3);
    const aiAnimMesh *target = mesh->mAnimMeshes[0];

    const V3 expectedNormal = {0.0f, -3.0f / kSqrt58, 7.0f / kSqrt58};
    const V3 expectedTangent = {1.0f, 0.0f, 0.0f};
    for (unsigned int v = 0; v < 3; ++v) {
        TASK_CHECK(NearV(target->mNormals[v], expectedNormal, 1e-5f));
        TASK_CHECK(NearV(target->mTangents[v], expectedTangent, 1e-5f));

        // Independent frame relation: B = N x T (w = +1).
        const V3 rebuiltBitangent = Cross(Of(target->mNormals[v]), Of(target->mTangents[v]));
        TASK_CHECK(NearV(target->mBitangents[v], Normalized(rebuiltBitangent), 1e-5f));
    }

    // Base frame differs from the target frame (non-uniform deformation).
    TASK_CHECK(!NearV(mesh->mNormals[0], Of(target->mNormals[0]), 1e-3f));
    return 0;
}

// Option and process-flag gating: the option alone changes nothing; each
// process flag still decides which stream is produced.
static int GenerationGating() {
    // Option disabled: base streams are generated but targets stay empty.
    Loaded off = LoadGlTF(kSceneNeighborhoods, false, kBothFlags);
    TASK_CHECK(off.scene != nullptr);
    const aiAnimMesh *offTarget = off.scene->mMeshes[0]->mAnimMeshes[0];
    TASK_CHECK(off.scene->mMeshes[0]->HasNormals());
    TASK_CHECK(!offTarget->HasNormals());
    TASK_CHECK(offTarget->mTangents == nullptr);

    // Normals flag only: target normals appear, tangents do not.
    Loaded normalsOnly = LoadGlTF(kSceneNeighborhoods, true, aiProcess_GenSmoothNormals);
    TASK_CHECK(normalsOnly.scene != nullptr);
    const aiAnimMesh *nTarget = normalsOnly.scene->mMeshes[0]->mAnimMeshes[0];
    TASK_CHECK(nTarget->HasNormals());
    TASK_CHECK(nTarget->mTangents == nullptr);
    TASK_CHECK(nTarget->mBitangents == nullptr);

    // Tangent flag without normals: nothing can be built without normals.
    Loaded tangentOnly = LoadGlTF(kSceneNeighborhoods, true, aiProcess_CalcTangentSpace);
    TASK_CHECK(tangentOnly.scene != nullptr);
    const aiAnimMesh *tTarget = tangentOnly.scene->mMeshes[0]->mAnimMeshes[0];
    TASK_CHECK(!tTarget->HasNormals());
    TASK_CHECK(tTarget->mTangents == nullptr);
    return 0;
}

// ===========================================================================
// ownership group
// ===========================================================================

// Artist-authored normal and tangent arrays survive completion untouched,
// even while the other stream class would otherwise be generated.
static int OwnershipPreservesArtistStreams() {
    Loaded loaded = LoadGlTF(kScenePreservation, true, kBothFlags);
    TASK_CHECK(loaded.scene != nullptr);
    const aiMesh *mesh = loaded.scene->mMeshes[0];
    TASK_CHECK(mesh->mNumAnimMeshes == 3);

    const aiAnimMesh *artist = mesh->mAnimMeshes[0];
    // Imported artist values: normal (0,0.6,1.8), tangent (1.1,0,0).
    for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
        TASK_CHECK(NearV(artist->mNormals[v], {0.0f, 0.6f, 1.8f}, 1e-6f));
        TASK_CHECK(NearV(artist->mTangents[v], {1.1f, 0.0f, 0.0f}, 1e-6f));
    }

    // Base streams keep their own identity and values.
    TASK_CHECK(NearV(mesh->mNormals[0], {0.0f, 0.0f, 1.0f}, 1e-6f));
    TASK_CHECK(NearV(mesh->mTangents[0], {1.0f, 0.0f, 0.0f}, 1e-6f));
    return 0;
}

// Targets missing one stream class get exactly that class completed while
// present arrays are retained; tangent and bitangent always appear together.
static int OwnershipFillsMissingStreamOnly() {
    Loaded loaded = LoadGlTF(kScenePreservation, true, kBothFlags);
    TASK_CHECK(loaded.scene != nullptr);
    const aiMesh *mesh = loaded.scene->mMeshes[0];

    // Target 1: positions only -> both normals and tangent pair generated.
    const aiAnimMesh *positionsOnly = mesh->mAnimMeshes[1];
    TASK_CHECK(positionsOnly->HasPositions());
    TASK_CHECK(positionsOnly->HasNormals());
    TASK_CHECK(positionsOnly->mTangents != nullptr && positionsOnly->mBitangents != nullptr);
    TASK_CHECK(NearV(positionsOnly->mNormals[0], {0.0f, 0.0f, 1.0f}));

    // Target 2: artist normals only -> normals retained, tangent pair added.
    const aiAnimMesh *normalsOnly = mesh->mAnimMeshes[2];
    TASK_CHECK(normalsOnly->HasNormals());
    TASK_CHECK(!normalsOnly->HasPositions());
    TASK_CHECK(normalsOnly->mTangents != nullptr && normalsOnly->mBitangents != nullptr);
    TASK_CHECK(NearV(normalsOnly->mNormals[0], {0.0f, -0.2f, 1.1f}, 1e-6f));
    TASK_CHECK(NearV(normalsOnly->mTangents[0], {1.0f, 0.0f, 0.0f}));
    // Tangent projected against the tilted artist normal.
    TASK_CHECK(NearV(normalsOnly->mBitangents[0], {0.0f, 0.97473f, 0.22338f}, 1e-4f));
    return 0;
}

// Re-running the pipeline does not rebuild existing target arrays and leaves
// every value and the base streams stable.
static int OwnershipRepeatedProcessing() {
    Loaded loaded = LoadGlTF(kSceneNeighborhoods, true, kBothFlags);
    TASK_CHECK(loaded.scene != nullptr);
    const aiMesh *first = loaded.scene->mMeshes[0];
    aiAnimMesh *target = first->mAnimMeshes[0];

    const aiVector3D *baseNormalPtr = first->mNormals;
    const aiVector3D *targetNormalPtr = target->mNormals;
    const aiVector3D *targetTangentPtr = target->mTangents;
    const aiVector3D *targetBitangentPtr = target->mBitangents;
    const aiVector3D normalBefore = target->mNormals[3];
    const aiVector3D tangentBefore = target->mTangents[3];

    const aiScene *again = loaded.importer->ApplyPostProcessing(kBothFlags);
    TASK_CHECK(again != nullptr);
    const aiMesh *second = again->mMeshes[0];
    TASK_CHECK(second->mNormals == baseNormalPtr);
    aiAnimMesh *againTarget = second->mAnimMeshes[0];
    TASK_CHECK(againTarget->mNormals == targetNormalPtr);
    TASK_CHECK(againTarget->mTangents == targetTangentPtr);
    TASK_CHECK(againTarget->mBitangents == targetBitangentPtr);
    TASK_CHECK(NearV(againTarget->mNormals[3], Of(normalBefore), 1e-6f));
    TASK_CHECK(NearV(againTarget->mTangents[3], Of(tangentBefore), 1e-6f));
    return 0;
}

// Ordering, names, weights and array ownership: targets own distinct arrays,
// and destroying one target must not touch the base streams or its sibling.
static int OwnershipIsolationAndMetadata() {
    Loaded loaded = LoadGlTF(kSceneTwoExpressions, true, kBothFlags);
    TASK_CHECK(loaded.scene != nullptr);
    aiMesh *mesh = loaded.scene->mMeshes[0];
    TASK_CHECK(mesh->mNumAnimMeshes == 2);

    aiAnimMesh *t0 = mesh->mAnimMeshes[0];
    aiAnimMesh *t1 = mesh->mAnimMeshes[1];

    // Names and weights survive in target order.
    TASK_CHECK(std::strcmp(t0->mName.C_Str(), "brow_raise") == 0);
    TASK_CHECK(std::strcmp(t1->mName.C_Str(), "jaw_open") == 0);
    TASK_CHECK(Near(t0->mWeight, 1.0f));
    TASK_CHECK(Near(t1->mWeight, 1.0f));

    // No target aliases the base streams or its sibling.
    TASK_CHECK(t0->mNormals != nullptr && t0->mNormals != mesh->mNormals);
    TASK_CHECK(t1->mNormals != nullptr && t1->mNormals != mesh->mNormals);
    TASK_CHECK(t0->mNormals != t1->mNormals);
    TASK_CHECK(t0->mTangents != t1->mTangents);

    const aiVector3D *baseNormalPtr = mesh->mNormals;
    const aiVector3D *baseTangentPtr = mesh->mTangents;
    const V3 siblingNormal = Of(t0->mNormals[0]);

    // Destroy one target; the base streams and the surviving sibling remain.
    delete t1;
    mesh->mAnimMeshes[1] = nullptr;
    TASK_CHECK(mesh->mNormals == baseNormalPtr);
    TASK_CHECK(mesh->mTangents == baseTangentPtr);
    TASK_CHECK(NearV(mesh->mNormals[0], {0.0f, 0.0f, 1.0f}));
    TASK_CHECK(NearV(t0->mNormals[0], siblingNormal, 1e-6f));
    TASK_CHECK(t0->mTangents != nullptr && t0->mBitangents != nullptr);
    return 0;
}

// ===========================================================================
// gltf group
// ===========================================================================

namespace {

// Exports the current scene with the requested morph target switches and
// opens the parsed JSON plus decoded binary body.
bool ExportAndOpen(const aiScene *scene, const std::string &fileName, bool exportNormals,
        bool exportTangents, std::unique_ptr<GltfFile> &out, bool exportNames = false) {
    Assimp::ExportProperties properties;
    if (exportNormals) {
        properties.SetPropertyBool("GLTF2_TARGET_NORMAL_EXP", true);
    }
    if (exportTangents) {
        properties.SetPropertyBool(AI_CONFIG_GLTF2_TARGET_TANGENT_EXP, true);
    }
    if (exportNames) {
        properties.SetPropertyBool("GLTF2_TARGETNAMES_EXP", true);
    }
    Assimp::Exporter exporter;
    const std::string path = WorkDir() + "/" + fileName;
    if (exporter.Export(scene, "gltf2", path.c_str(), 0u, &properties) != aiReturn_SUCCESS) {
        return false;
    }
    return OpenGltfFile(path, out);
}

} // namespace

// With the switch on, each target gets a plain FLOAT/VEC3 TANGENT accessor
// holding target tangent xyz minus base tangent xyz; base w and order remain.
static int GltfStoresTangentDifferences() {
    Loaded loaded = LoadGlTF(kSceneTwoExpressions, true, kBothFlags);
    TASK_CHECK(loaded.scene != nullptr);
    const aiMesh *mesh = loaded.scene->mMeshes[0];

    std::unique_ptr<GltfFile> file;
    TASK_CHECK(ExportAndOpen(loaded.scene, "morph_task_tangent.gltf", true, true, file));

    const auto &primitive = file->doc["meshes"][0]["primitives"][0];
    const auto &targets = primitive["targets"];
    TASK_CHECK(targets.Size() == mesh->mNumAnimMeshes);

    const unsigned int baseTangentAccessor = primitive["attributes"]["TANGENT"].GetInt();
    std::vector<std::array<float, 4>> baseTangents;
    TASK_CHECK(DecodeAccessor(*file, baseTangentAccessor, 4, baseTangents));
    for (const auto &tangent : baseTangents) {
        TASK_CHECK(Near(tangent[3], 1.0f, 1e-6f)); // handedness preserved
    }

    for (rapidjson::SizeType t = 0; t < targets.Size(); ++t) {
        TASK_CHECK(targets[t].HasMember("TANGENT"));
        const unsigned int tangentAccessor = targets[t]["TANGENT"].GetInt();
        std::vector<std::array<float, 4>> stored;
        TASK_CHECK(DecodeAccessor(*file, tangentAccessor, 3, stored));

        const aiAnimMesh *target = mesh->mAnimMeshes[t];
        TASK_CHECK(stored.size() == target->mNumVertices);
        for (size_t v = 0; v < stored.size(); ++v) {
            // Independently decode the bytes and confirm they are the diff.
            TASK_CHECK(Near(stored[v][0], target->mTangents[v].x - mesh->mTangents[v].x, 1e-6f));
            TASK_CHECK(Near(stored[v][1], target->mTangents[v].y - mesh->mTangents[v].y, 1e-6f));
            TASK_CHECK(Near(stored[v][2], target->mTangents[v].z - mesh->mTangents[v].z, 1e-6f));
        }
    }
    return 0;
}

// With the new switch off, no TANGENT accessor is written and the legacy
// normal/position behavior is unchanged.
static int GltfSwitchOffKeepsLegacyOutput() {
    Loaded loaded = LoadGlTF(kSceneTwoExpressions, true, kBothFlags);
    TASK_CHECK(loaded.scene != nullptr);

    std::unique_ptr<GltfFile> normalOnly;
    TASK_CHECK(ExportAndOpen(loaded.scene, "morph_task_normal_only.gltf", true, false, normalOnly));
    const auto &normalTargets = normalOnly->doc["meshes"][0]["primitives"][0]["targets"];
    for (rapidjson::SizeType t = 0; t < normalTargets.Size(); ++t) {
        TASK_CHECK(normalTargets[t].HasMember("NORMAL"));
        TASK_CHECK(normalTargets[t].HasMember("POSITION"));
        TASK_CHECK(!normalTargets[t].HasMember("TANGENT"));
    }

    std::unique_ptr<GltfFile> legacy;
    TASK_CHECK(ExportAndOpen(loaded.scene, "morph_task_legacy.gltf", false, false, legacy));
    const auto &legacyTargets = legacy->doc["meshes"][0]["primitives"][0]["targets"];
    for (rapidjson::SizeType t = 0; t < legacyTargets.Size(); ++t) {
        TASK_CHECK(legacyTargets[t].HasMember("POSITION"));
        TASK_CHECK(!legacyTargets[t].HasMember("NORMAL"));
        TASK_CHECK(!legacyTargets[t].HasMember("TANGENT"));
    }
    return 0;
}

// Re-import restores endpoint normals and tangents. The bitangent is rebuilt
// by the test itself as w * (N x T) rather than through the same read/write
// logic, so the round-trip is checked independently.
static int GltfReimportRestoresEndpoints() {
    Loaded loaded = LoadGlTF(kSceneTwoExpressions, true, kBothFlags);
    TASK_CHECK(loaded.scene != nullptr);

    std::unique_ptr<GltfFile> file;
    TASK_CHECK(ExportAndOpen(loaded.scene, "morph_task_roundtrip.gltf", true, true, file,
                    true /* keep target names across the round trip */));

    Assimp::Importer reimporter;
    const std::string path = WorkDir() + "/morph_task_roundtrip.gltf";
    const aiScene *reimported = reimporter.ReadFile(path.c_str(), 0);
    TASK_CHECK(reimported != nullptr);
    const aiMesh *mesh = reimported->mMeshes[0];
    TASK_CHECK(mesh->mNumAnimMeshes == 2);

    // Read the base tangent handedness straight from the exported bytes; the
    // base tangent w is preserved by the new path.
    const unsigned int baseTangentAccessor =
            file->doc["meshes"][0]["primitives"][0]["attributes"]["TANGENT"].GetInt();
    std::vector<std::array<float, 4>> baseTangents;
    TASK_CHECK(DecodeAccessor(*file, baseTangentAccessor, 4, baseTangents));

    // Hand-derived endpoint frames for the two expressions.
    const V3 expectedNormals[2] = {
            {-1.0f / kSqrt5, 0.0f, 2.0f / kSqrt5},
            {-2.0f / 3.0f, -1.0f / 3.0f, 2.0f / 3.0f}};
    const V3 expectedTangents[2] = {
            {2.0f / kSqrt5, 0.0f, 1.0f / kSqrt5},
            {1.0f / kSqrt2, 0.0f, 1.0f / kSqrt2}};

    for (unsigned int t = 0; t < mesh->mNumAnimMeshes; ++t) {
        const aiAnimMesh *target = mesh->mAnimMeshes[t];
        TASK_CHECK(target->HasNormals());
        TASK_CHECK(target->HasTangentsAndBitangents());
        TASK_CHECK(std::strcmp(target->mName.C_Str(), t == 0 ? "brow_raise" : "jaw_open") == 0);

        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            TASK_CHECK(NearV(target->mNormals[v], expectedNormals[t], 1e-4f));
            TASK_CHECK(NearV(target->mTangents[v], expectedTangents[t], 1e-4f));

            // Independent bitangent: w * (N x T), using the exported w rather
            // than any value produced by the importer's own reconstruction.
            const float handedness = baseTangents[v][3];
            TASK_CHECK(Near(handedness, 1.0f, 1e-6f));
            const V3 expectedBitangent = Cross(Of(target->mNormals[v]), Of(target->mTangents[v]));
            const V3 scaled{expectedBitangent.x * handedness, expectedBitangent.y * handedness,
                    expectedBitangent.z * handedness};
            TASK_CHECK(NearV(target->mBitangents[v], Normalized(scaled), 1e-4f));
        }
    }
    return 0;
}

// ===========================================================================
// registration
// ===========================================================================

const struct task_case task_cases[] = {
        {"generation", "completes_all_targets", &GenerationCompletesAllTargets},
        {"generation", "per_target_neighborhoods", &GenerationPerTargetNeighborhoods},
        {"generation", "slanted_triangle_analytic", &GenerationSlantedTriangleAnalytic},
        {"generation", "option_and_flag_gating", &GenerationGating},

        {"ownership", "preserves_artist_streams", &OwnershipPreservesArtistStreams},
        {"ownership", "fills_missing_stream_only", &OwnershipFillsMissingStreamOnly},
        {"ownership", "repeated_processing", &OwnershipRepeatedProcessing},
        {"ownership", "isolation_and_metadata", &OwnershipIsolationAndMetadata},

        {"gltf", "stores_tangent_differences", &GltfStoresTangentDifferences},
        {"gltf", "switch_off_keeps_legacy_output", &GltfSwitchOffKeepsLegacyOutput},
        {"gltf", "reimport_restores_endpoints", &GltfReimportRestoresEndpoints},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
