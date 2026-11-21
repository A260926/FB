// -----------------------------------------------------------------------------
// Donor/scratch: this file is the task regression suite for ChangeSceneBasis.
// It intentionally defines no main(); task_support/driver.c provides it.
// -----------------------------------------------------------------------------

#include "PostProcessing/SceneBasisProcess.h"
#include "PostProcessing/ScaleProcess.h"
#include "task_support/check.h"

#include <assimp/Exceptional.h>
#include <assimp/Importer.hpp>
#include <assimp/anim.h>
#include <assimp/config.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/types.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Small numeric helpers
// ---------------------------------------------------------------------------

constexpr ai_real kEps = static_cast<ai_real>(2e-4);

// Bitwise equality helpers, used where a value may legitimately be NaN.
bool ExactMat(const aiMatrix4x4 &a, const aiMatrix4x4 &b) {
    return std::memcmp(&a, &b, sizeof(aiMatrix4x4)) == 0;
}

bool ExactVec(const aiVector3D &a, const aiVector3D &b) {
    return std::memcmp(&a, &b, sizeof(aiVector3D)) == 0;
}

bool Near(ai_real a, ai_real b, ai_real eps = kEps) {
    return std::fabs(static_cast<double>(a - b)) <= static_cast<double>(eps);
}

bool NearVec(const aiVector3D &a, const aiVector3D &b, ai_real eps = kEps) {
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

bool NearMat(const aiMatrix4x4 &a, const aiMatrix4x4 &b, ai_real eps = kEps) {
    return Near(a.a1, b.a1, eps) && Near(a.a2, b.a2, eps) && Near(a.a3, b.a3, eps) && Near(a.a4, b.a4, eps) &&
           Near(a.b1, b.b1, eps) && Near(a.b2, b.b2, eps) && Near(a.b3, b.b3, eps) && Near(a.b4, b.b4, eps) &&
           Near(a.c1, b.c1, eps) && Near(a.c2, b.c2, eps) && Near(a.c3, b.c3, eps) && Near(a.c4, b.c4, eps) &&
           Near(a.d1, b.d1, eps) && Near(a.d2, b.d2, eps) && Near(a.d3, b.d3, eps) && Near(a.d4, b.d4, eps);
}

aiMatrix4x4 MatIdentity() {
    return aiMatrix4x4();
}

aiMatrix4x4 ShearTranslationBasis() {
    aiMatrix4x4 b;
    b.a1 = 1.25f;
    b.a2 = 0.35f;
    b.a4 = 2.0f;
    b.b2 = 0.8f;
    b.b4 = -1.5f;
    b.c3 = 1.1f;
    b.c4 = 0.25f;
    return b;
}

aiMatrix4x4 ReflectionBasis() {
    aiMatrix4x4 b;
    b.a1 = -1.2f;
    b.a2 = 0.2f;
    b.a4 = 0.5f;
    b.b2 = 1.4f;
    b.b3 = 0.15f;
    b.c3 = 0.9f;
    b.c4 = -0.4f;
    return b;
}

aiMatrix4x4 UniformScaledBasis(ai_real s) {
    aiMatrix4x4 b;
    b.a1 = s;
    b.b2 = s;
    b.c3 = s;
    return b;
}

aiMatrix4x4 Translation(ai_real x, ai_real y, ai_real z) {
    aiMatrix4x4 m;
    m.a4 = x;
    m.b4 = y;
    m.c4 = z;
    return m;
}

aiMatrix4x4 RotationZ(ai_real angle) {
    aiMatrix4x4 m;
    const ai_real c = std::cos(static_cast<double>(angle));
    const ai_real s = std::sin(static_cast<double>(angle));
    m.a1 = c;
    m.a2 = -s;
    m.b1 = s;
    m.b2 = c;
    return m;
}

aiMatrix4x4 RotationX(ai_real angle) {
    aiMatrix4x4 m;
    const ai_real c = std::cos(static_cast<double>(angle));
    const ai_real s = std::sin(static_cast<double>(angle));
    m.b2 = c;
    m.b3 = -s;
    m.c2 = s;
    m.c3 = c;
    return m;
}

aiMatrix4x4 Inverse(const aiMatrix4x4 &m) {
    aiMatrix4x4 copy = m;
    copy.Inverse();
    return copy;
}

aiVector3D Normalized(const aiVector3D &v) {
    aiVector3D copy = v;
    copy.NormalizeSafe();
    return copy;
}

// ---------------------------------------------------------------------------
// Independent animation sampler used by the tests (deliberately separate from
// the production code). It evaluates the world matrix of every node exactly as
// the assimp aiNodeAnim TRS rules describe them.
// ---------------------------------------------------------------------------

aiVector3D SampleVectorTrack(const aiVectorKey *keys, unsigned int count,
        const aiVector3D &defaultValue, aiAnimBehaviour pre, aiAnimBehaviour post, double time) {
    if (count == 0) {
        return defaultValue;
    }
    if (time < keys[0].mTime) {
        switch (pre) {
        case aiAnimBehaviour_DEFAULT:
            return defaultValue;
        case aiAnimBehaviour_LINEAR:
            // Extrapolate using the first two keys.
            if (count >= 2 && keys[1].mTime > keys[0].mTime) {
                const double f = (time - keys[0].mTime) / (keys[1].mTime - keys[0].mTime);
                return keys[0].mValue + (keys[1].mValue - keys[0].mValue) * static_cast<ai_real>(f);
            }
            return keys[0].mValue;
        case aiAnimBehaviour_REPEAT: {
            const double span = keys[count - 1].mTime - keys[0].mTime;
            if (span > 0.0) {
                double wrapped = std::fmod(time - keys[0].mTime, span);
                if (wrapped < 0.0) {
                    wrapped += span;
                }
                return SampleVectorTrack(keys, count, defaultValue, aiAnimBehaviour_CONSTANT,
                        aiAnimBehaviour_CONSTANT, keys[0].mTime + wrapped);
            }
            return keys[0].mValue;
        }
        case aiAnimBehaviour_CONSTANT:
        default:
            return keys[0].mValue;
        }
    }
    if (time > keys[count - 1].mTime) {
        switch (post) {
        case aiAnimBehaviour_DEFAULT:
            return defaultValue;
        case aiAnimBehaviour_LINEAR:
            if (count >= 2 && keys[count - 1].mTime > keys[count - 2].mTime) {
                const double f = (time - keys[count - 1].mTime) /
                                 (keys[count - 1].mTime - keys[count - 2].mTime);
                return keys[count - 1].mValue +
                       (keys[count - 1].mValue - keys[count - 2].mValue) * static_cast<ai_real>(f);
            }
            return keys[count - 1].mValue;
        case aiAnimBehaviour_REPEAT: {
            const double span = keys[count - 1].mTime - keys[0].mTime;
            if (span > 0.0) {
                double wrapped = std::fmod(time - keys[0].mTime, span);
                if (wrapped < 0.0) {
                    wrapped += span;
                }
                return SampleVectorTrack(keys, count, defaultValue, aiAnimBehaviour_CONSTANT,
                        aiAnimBehaviour_CONSTANT, keys[0].mTime + wrapped);
            }
            return keys[count - 1].mValue;
        }
        case aiAnimBehaviour_CONSTANT:
        default:
            return keys[count - 1].mValue;
        }
    }
    for (unsigned int i = 1; i < count; ++i) {
        if (time <= keys[i].mTime) {
            const double span = keys[i].mTime - keys[i - 1].mTime;
            const double f = span > 0.0 ? (time - keys[i - 1].mTime) / span : 0.0;
            const ai_real rf = static_cast<ai_real>(f);
            return keys[i - 1].mValue * (static_cast<ai_real>(1) - rf) + keys[i].mValue * rf;
        }
    }
    return keys[count - 1].mValue;
}

aiQuaternion SampleQuatTrack(const aiQuatKey *keys, unsigned int count,
        const aiQuaternion &defaultValue, aiAnimBehaviour pre, aiAnimBehaviour post, double time) {
    if (count == 0) {
        return defaultValue;
    }
    if (time < keys[0].mTime) {
        if (pre == aiAnimBehaviour_DEFAULT) {
            return defaultValue;
        }
        return keys[0].mValue;
    }
    if (time > keys[count - 1].mTime) {
        if (post == aiAnimBehaviour_DEFAULT) {
            return defaultValue;
        }
        return keys[count - 1].mValue;
    }
    for (unsigned int i = 1; i < count; ++i) {
        if (time <= keys[i].mTime) {
            const double span = keys[i].mTime - keys[i - 1].mTime;
            const double f = span > 0.0 ? (time - keys[i - 1].mTime) / span : 0.0;
            aiQuaternion result;
            aiQuaternion::Interpolate(result, keys[i - 1].mValue, keys[i].mValue,
                    static_cast<ai_real>(f));
            result.Normalize();
            return result;
        }
    }
    return keys[count - 1].mValue;
}

struct LocalSample {
    aiVector3D position;
    aiQuaternion rotation;
    aiVector3D scaling;
    bool animated = false;
};

LocalSample SampleLocalByChannel(const aiNodeAnim *channel, const aiMatrix4x4 &defaultTransform,
        double time) {
    LocalSample result;
    defaultTransform.Decompose(result.scaling, result.rotation, result.position);
    result.animated = true;

    result.position = SampleVectorTrack(channel->mPositionKeys, channel->mNumPositionKeys,
            result.position, channel->mPreState, channel->mPostState, time);
    result.rotation = SampleQuatTrack(channel->mRotationKeys, channel->mNumRotationKeys,
            result.rotation, channel->mPreState, channel->mPostState, time);
    result.scaling = SampleVectorTrack(channel->mScalingKeys, channel->mNumScalingKeys,
            result.scaling, channel->mPreState, channel->mPostState, time);
    return result;
}

// Evaluates the local transform of a node. Channels are looked up by name in
// the supplied animation; if the node has no channel its static transform is
// used.
aiMatrix4x4 SampleLocal(const aiNode &node, const aiAnimation *animation, double time) {
    if (animation != nullptr) {
        for (unsigned int c = 0; c < animation->mNumChannels; ++c) {
            const aiNodeAnim *channel = animation->mChannels[c];
            if (channel->mNodeName != node.mName) {
                continue;
            }
            const LocalSample sample = SampleLocalByChannel(channel, node.mTransformation, time);
            return aiMatrix4x4(sample.scaling, sample.rotation, sample.position);
        }
    }
    return node.mTransformation;
}

aiMatrix4x4 SampleWorld(const aiNode &node, const aiAnimation *animation, double time) {
    const aiMatrix4x4 local = SampleLocal(node, animation, time);
    if (node.mParent != nullptr) {
        return SampleWorld(*node.mParent, animation, time) * local;
    }
    return local;
}

// Samples the world matrix of a semantic node identified by its original
// name. The node object is found by name (its local transform may have been
// changed, but names are preserved).
const aiNode *FindUniqueNode(const aiNode *root, const char *name) {
    if (root == nullptr) {
        return nullptr;
    }
    const aiNode *found = nullptr;
    if (std::strcmp(root->mName.C_Str(), name) == 0) {
        found = root;
    }
    for (unsigned int i = 0; i < root->mNumChildren; ++i) {
        const aiNode *child = FindUniqueNode(root->mChildren[i], name);
        if (child != nullptr) {
            if (found != nullptr) {
                return nullptr; // ambiguous
            }
            found = child;
        }
    }
    return found;
}

// Finds the node that owns (references) the given mesh index.
const aiNode *FindMeshOwner(const aiNode *root, unsigned int meshIndex) {
    if (root == nullptr) {
        return nullptr;
    }
    for (unsigned int i = 0; i < root->mNumMeshes; ++i) {
        if (root->mMeshes[i] == meshIndex) {
            return root;
        }
    }
    for (unsigned int i = 0; i < root->mNumChildren; ++i) {
        const aiNode *child = FindMeshOwner(root->mChildren[i], meshIndex);
        if (child != nullptr) {
            return child;
        }
    }
    return nullptr;
}

// Collects all node pointers in a tree.
void CollectNodePtrs(const aiNode *node, std::vector<const aiNode *> &out) {
    if (node == nullptr) {
        return;
    }
    out.push_back(node);
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        CollectNodePtrs(node->mChildren[i], out);
    }
}

void CollectNodeNames(const aiNode *node, std::vector<std::string> &out) {
    if (node == nullptr) {
        return;
    }
    out.emplace_back(node->mName.C_Str());
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        CollectNodeNames(node->mChildren[i], out);
    }
}

unsigned int CountNodes(const aiNode *node) {
    if (node == nullptr) {
        return 0;
    }
    unsigned int count = 1;
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        count += CountNodes(node->mChildren[i]);
    }
    return count;
}

// ---------------------------------------------------------------------------
// Scene construction helpers
// ---------------------------------------------------------------------------

struct VertexWeightSpec {
    unsigned int vertexId;
    ai_real weight;
};

aiBone *MakeBone(const char *name, const aiMatrix4x4 &offset,
        const std::vector<VertexWeightSpec> &weights) {
    aiBone *bone = new aiBone();
    bone->mName = name;
    bone->mOffsetMatrix = offset;
    bone->mNumWeights = static_cast<unsigned int>(weights.size());
    bone->mWeights = weights.empty() ? nullptr : new aiVertexWeight[weights.size()];
    for (std::size_t i = 0; i < weights.size(); ++i) {
        bone->mWeights[i].mVertexId = weights[i].vertexId;
        bone->mWeights[i].mWeight = weights[i].weight;
    }
    return bone;
}

aiAnimMesh *MakeAnimMesh(const char *name, unsigned int vertexCount, ai_real weight) {
    aiAnimMesh *anim = new aiAnimMesh();
    anim->mName = name;
    anim->mNumVertices = vertexCount;
    anim->mWeight = weight;
    return anim;
}

aiVectorKey *MakeVectorKeys(int start, int count, double step, ai_real baseX, ai_real baseY,
        ai_real baseZ, ai_real deltaX, ai_real deltaY, ai_real deltaZ) {
    aiVectorKey *keys = new aiVectorKey[static_cast<std::size_t>(count)];
    for (int i = 0; i < count; ++i) {
        const ai_real t = static_cast<ai_real>(i);
        keys[i].mTime = static_cast<double>(start + i) * step;
        keys[i].mValue = aiVector3D(baseX + deltaX * t, baseY + deltaY * t, baseZ + deltaZ * t);
        keys[i].mInterpolation = aiAnimInterpolation_Linear;
    }
    return keys;
}

aiQuatKey *MakeRotationKeys(int start, int count, double step, ai_real totalAngle) {
    aiQuatKey *keys = new aiQuatKey[static_cast<std::size_t>(count)];
    for (int i = 0; i < count; ++i) {
        const ai_real t = static_cast<ai_real>(i) / static_cast<ai_real>(count - 1);
        const ai_real angle = totalAngle * t;
        keys[i].mTime = static_cast<double>(start + i) * step;
        keys[i].mValue = aiQuaternion(aiVector3D(0.0f, 0.0f, 1.0f), angle);
        keys[i].mInterpolation = aiAnimInterpolation_Spherical_Linear;
    }
    return keys;
}

// Canonical values for a channel at each key, used to verify that keys are
// unchanged by the change of basis.
struct ChannelSignature {
    std::vector<double> times;
    std::vector<aiVector3D> positions;
    std::vector<aiQuaternion> rotations;
    std::vector<aiVector3D> scalings;
    aiAnimBehaviour pre = aiAnimBehaviour_DEFAULT;
    aiAnimBehaviour post = aiAnimBehaviour_DEFAULT;
    std::string nodeName;
};

ChannelSignature CaptureChannel(const aiNodeAnim &channel) {
    ChannelSignature sig;
    sig.nodeName = channel.mNodeName.C_Str();
    sig.pre = channel.mPreState;
    sig.post = channel.mPostState;
    for (unsigned int i = 0; i < channel.mNumPositionKeys; ++i) {
        sig.times.push_back(channel.mPositionKeys[i].mTime);
        sig.positions.push_back(channel.mPositionKeys[i].mValue);
    }
    for (unsigned int i = 0; i < channel.mNumRotationKeys; ++i) {
        sig.times.push_back(channel.mRotationKeys[i].mTime);
        sig.rotations.push_back(channel.mRotationKeys[i].mValue);
    }
    for (unsigned int i = 0; i < channel.mNumScalingKeys; ++i) {
        sig.times.push_back(channel.mScalingKeys[i].mTime);
        sig.scalings.push_back(channel.mScalingKeys[i].mValue);
    }
    return sig;
}

bool SameKeys(const ChannelSignature &a, const ChannelSignature &b) {
    if (a.positions.size() != b.positions.size() || a.rotations.size() != b.rotations.size() ||
            a.scalings.size() != b.scalings.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.positions.size(); ++i) {
        if (a.positions[i] != b.positions[i]) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.rotations.size(); ++i) {
        if (a.rotations[i] != b.rotations[i]) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.scalings.size(); ++i) {
        if (a.scalings[i] != b.scalings[i]) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// A rich, two-bone LBS scene with a shared mesh, sparse morphs and several
// animations. It is the workhorse fixture for most checks.
// ---------------------------------------------------------------------------

struct RichFixture {
    aiScene *scene = nullptr;

    aiMesh *mesh = nullptr;
    aiNode *root = nullptr;
    aiNode *armature = nullptr;
    aiNode *hip = nullptr;
    aiNode *knee = nullptr;
    aiNode *instanceA = nullptr;
    aiNode *instanceB = nullptr;
    aiNode *staticHelper = nullptr;
    aiNode *staticChild = nullptr;

    aiAnimation *walk = nullptr;
    aiAnimation *idle = nullptr;
    aiAnimation *oneShot = nullptr;

    aiBone *hipBone = nullptr;
    aiBone *kneeBone = nullptr;

    ~RichFixture() {
        delete scene;
        scene = nullptr;
    }
};

// Builds a scene:
//   Root
//     Armature                      (static, non-uniform scale)
//       Hip                         (animated, rotation + translation)
//         Knee                      (animated, rotation)
//     InstanceA                     (mesh 0, static translation)
//     InstanceB                     (mesh 0, static translation + rotation)
//     StaticHelper                  (no mesh, static)
//       StaticChild                 (no mesh, animated only in walk)
//
// The mesh has two bones Hip/Knee, four vertices with rigid LBS weights and
// two morph targets (one overriding only positions, one only normals).
RichFixture *BuildRichFixture() {
    auto *fixture = new RichFixture();
    aiScene *scene = new aiScene();
    fixture->scene = scene;

    fixture->root = new aiNode("Root");
    scene->mRootNode = fixture->root;
    fixture->armature = new aiNode("Armature");
    fixture->hip = new aiNode("Hip");
    fixture->knee = new aiNode("Knee");
    fixture->instanceA = new aiNode("InstanceA");
    fixture->instanceB = new aiNode("InstanceB");
    fixture->staticHelper = new aiNode("StaticHelper");
    fixture->staticChild = new aiNode("StaticChild");

    fixture->armature->mTransformation = aiMatrix4x4();
    fixture->armature->mTransformation.a1 = 1.1f;
    fixture->armature->mTransformation.b2 = 0.95f;
    fixture->armature->mTransformation.c3 = 1.05f;

    fixture->hip->mTransformation = Translation(0.5f, 1.0f, 0.25f);
    fixture->knee->mTransformation = Translation(1.5f, 0.0f, 0.0f);
    fixture->instanceA->mTransformation = Translation(2.0f, 0.0f, 0.0f);
    fixture->instanceB->mTransformation = RotationZ(0.6f) * Translation(-2.0f, 0.0f, 1.0f);
    fixture->staticHelper->mTransformation = Translation(0.0f, 3.0f, 0.0f);
    fixture->staticChild->mTransformation = RotationX(0.4f);

    // --- mesh -------------------------------------------------------------
    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh *[1]{ new aiMesh() };
    aiMesh *mesh = scene->mMeshes[0];
    fixture->mesh = mesh;
    mesh->mName = "BodyMesh";
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mMaterialIndex = 0;

    const aiVector3D basePositions[4] = {
        aiVector3D(0.0f, 0.0f, 0.0f),
        aiVector3D(1.0f, 0.0f, 0.0f),
        aiVector3D(0.0f, 1.0f, 0.0f),
        aiVector3D(1.0f, 1.0f, 1.0f)
    };
    const aiVector3D baseNormals[4] = {
        aiVector3D(0.0f, 0.0f, 1.0f),
        aiVector3D(0.0f, 0.0f, 1.0f),
        aiVector3D(0.0f, 1.0f, 0.0f),
        aiVector3D(0.0f, 1.0f, 0.0f)
    };
    mesh->mNumVertices = 4;
    mesh->mVertices = new aiVector3D[4];
    mesh->mNormals = new aiVector3D[4];
    mesh->mTangents = new aiVector3D[4];
    mesh->mBitangents = new aiVector3D[4];
    mesh->mColors[0] = new aiColor4D[4];
    mesh->mTextureCoords[0] = new aiVector3D[4];
    mesh->mNumUVComponents[0] = 2;
    for (unsigned int v = 0; v < 4; ++v) {
        mesh->mVertices[v] = basePositions[v];
        mesh->mNormals[v] = baseNormals[v];
        mesh->mTangents[v] = aiVector3D(1.0f, 0.0f, 0.0f);
        mesh->mBitangents[v] = aiVector3D(0.0f, 1.0f, 0.0f);
        mesh->mColors[0][v] = aiColor4D(0.2f, 0.4f, 0.6f, 1.0f);
        mesh->mTextureCoords[0][v] = aiVector3D(0.1f * static_cast<ai_real>(v), 0.25f, 0.0f);
    }

    mesh->mNumFaces = 2;
    mesh->mFaces = new aiFace[2];
    mesh->mFaces[0].mNumIndices = 3;
    mesh->mFaces[0].mIndices = new unsigned int[3]{ 0, 1, 2 };
    mesh->mFaces[1].mNumIndices = 3;
    mesh->mFaces[1].mIndices = new unsigned int[3]{ 1, 3, 2 };

    // --- bones ------------------------------------------------------------
    // Two-bone linear blend skinning: vertices 0/1 are weighted to Hip, 2/3 to
    // Knee. Weights sum to 1 for each affected vertex.
    aiMatrix4x4 hipBind = Translation(-0.25f, -0.5f, 0.0f);
    aiMatrix4x4 kneeBind = RotationZ(0.2f) * Translation(-1.0f, 0.0f, 0.0f);
    hipBind.a1 = 0.9f;
    hipBind.c3 = 1.2f;
    kneeBind.b2 = 1.3f;

    fixture->hipBone = MakeBone("Hip", hipBind, { { 0, 1.0f }, { 1, 1.0f } });
    fixture->kneeBone = MakeBone("Knee", kneeBind, { { 2, 1.0f }, { 3, 1.0f } });
    mesh->mNumBones = 2;
    mesh->mBones = new aiBone *[2]{ fixture->hipBone, fixture->kneeBone };

    // --- morph targets ----------------------------------------------------
    mesh->mNumAnimMeshes = 2;
    mesh->mAnimMeshes = new aiAnimMesh *[2];
    aiAnimMesh *posMorph = MakeAnimMesh("MorphPositions", 4, 0.5f);
    posMorph->mVertices = new aiVector3D[4]{
        aiVector3D(0.2f, 0.0f, 0.0f),
        aiVector3D(1.2f, 0.1f, 0.0f),
        aiVector3D(0.0f, 1.1f, 0.2f),
        aiVector3D(1.1f, 1.2f, 1.3f)
    };
    aiAnimMesh *normalMorph = MakeAnimMesh("MorphNormals", 4, 0.75f);
    normalMorph->mNormals = new aiVector3D[4]{
        aiVector3D(0.3f, 0.0f, 1.0f),
        aiVector3D(0.0f, 0.2f, 1.0f),
        aiVector3D(0.0f, 1.0f, 0.1f),
        aiVector3D(0.3f, 0.9f, 0.0f)
    };
    mesh->mAnimMeshes[0] = posMorph;
    mesh->mAnimMeshes[1] = normalMorph;

    // --- node hierarchy ---------------------------------------------------
    fixture->root->mNumChildren = 3;
    fixture->root->mChildren = new aiNode *[3]{ fixture->armature, fixture->instanceA,
            fixture->instanceB };
    fixture->root->mNumMeshes = 0;
    fixture->root->mMeshes = nullptr;
    fixture->armature->mParent = fixture->root;
    fixture->instanceA->mParent = fixture->root;
    fixture->instanceB->mParent = fixture->root;

    fixture->armature->mNumChildren = 1;
    fixture->armature->mChildren = new aiNode *[1]{ fixture->hip };
    fixture->hip->mParent = fixture->armature;

    fixture->hip->mNumChildren = 1;
    fixture->hip->mChildren = new aiNode *[1]{ fixture->knee };
    fixture->knee->mParent = fixture->hip;

    fixture->staticHelper->mParent = fixture->root;
    fixture->staticHelper->mNumChildren = 1;
    fixture->staticHelper->mChildren = new aiNode *[1]{ fixture->staticChild };
    fixture->staticChild->mParent = fixture->staticHelper;

    // Static helper nodes are attached, preserving sibling order.
    aiNode *originalChildren[4] = { fixture->armature, fixture->instanceA, fixture->instanceB,
        fixture->staticHelper };
    fixture->root->mNumChildren = 4;
    delete[] fixture->root->mChildren;
    fixture->root->mChildren = new aiNode *[4];
    for (unsigned int i = 0; i < 4; ++i) {
        fixture->root->mChildren[i] = originalChildren[i];
        originalChildren[i]->mParent = fixture->root;
    }

    fixture->instanceA->mNumMeshes = 1;
    fixture->instanceA->mMeshes = new unsigned int[1]{ 0 };
    fixture->instanceB->mNumMeshes = 1;
    fixture->instanceB->mMeshes = new unsigned int[1]{ 0 };

    // --- animations -------------------------------------------------------
    scene->mNumAnimations = 3;
    scene->mAnimations = new aiAnimation *[3];

    // walk: Hip (position + rotation + scaling), Knee (rotation).
    aiAnimation *walk = new aiAnimation();
    walk->mName = "walk";
    walk->mTicksPerSecond = 24.0;
    walk->mDuration = 24.0;
    walk->mNumChannels = 2;
    walk->mChannels = new aiNodeAnim *[2];
    aiNodeAnim *hipWalk = new aiNodeAnim();
    hipWalk->mNodeName = "Hip";
    hipWalk->mNumPositionKeys = 3;
    hipWalk->mPositionKeys = MakeVectorKeys(0, 3, 12.0, 0.5f, 1.0f, 0.25f, 0.4f, 0.2f, -0.1f);
    hipWalk->mNumRotationKeys = 3;
    hipWalk->mRotationKeys = MakeRotationKeys(0, 3, 12.0, 1.2f);
    hipWalk->mNumScalingKeys = 3;
    hipWalk->mScalingKeys = MakeVectorKeys(0, 3, 12.0, 1.0f, 1.0f, 1.0f, 0.05f, -0.02f, 0.03f);
    hipWalk->mPreState = aiAnimBehaviour_REPEAT;
    hipWalk->mPostState = aiAnimBehaviour_REPEAT;

    aiNodeAnim *kneeWalk = new aiNodeAnim();
    kneeWalk->mNodeName = "Knee";
    kneeWalk->mNumPositionKeys = 1;
    kneeWalk->mPositionKeys = new aiVectorKey[1]{ aiVectorKey(0.0, aiVector3D(1.5f, 0.0f, 0.0f)) };
    kneeWalk->mNumRotationKeys = 2;
    kneeWalk->mRotationKeys = MakeRotationKeys(0, 2, 24.0, -0.9f);
    kneeWalk->mNumScalingKeys = 1;
    kneeWalk->mScalingKeys = new aiVectorKey[1]{ aiVectorKey(0.0, aiVector3D(1.0f, 1.0f, 1.0f)) };
    kneeWalk->mPreState = aiAnimBehaviour_DEFAULT;
    kneeWalk->mPostState = aiAnimBehaviour_CONSTANT;

    walk->mChannels[0] = hipWalk;
    walk->mChannels[1] = kneeWalk;
    scene->mAnimations[0] = walk;
    fixture->walk = walk;

    // idle: only an Armature channel (rotation), so Hip/Knee/StaticChild use
    // their default transforms. Rotation channel is kept sparse on purpose.
    aiAnimation *idle = new aiAnimation();
    idle->mName = "idle";
    idle->mTicksPerSecond = 30.0;
    idle->mDuration = 60.0;
    idle->mNumChannels = 1;
    idle->mChannels = new aiNodeAnim *[1];
    aiNodeAnim *armatureIdle = new aiNodeAnim();
    armatureIdle->mNodeName = "Armature";
    armatureIdle->mNumPositionKeys = 2;
    armatureIdle->mPositionKeys = MakeVectorKeys(0, 2, 60.0, 0.0f, 0.0f, 0.0f, 0.0f, 0.1f, 0.0f);
    armatureIdle->mNumRotationKeys = 3;
    armatureIdle->mRotationKeys = MakeRotationKeys(0, 3, 30.0, 0.5f);
    armatureIdle->mNumScalingKeys = 2;
    armatureIdle->mScalingKeys = MakeVectorKeys(0, 2, 60.0, 1.0f, 1.0f, 1.0f, 0.1f, 0.0f, 0.0f);
    idle->mChannels[0] = armatureIdle;
    scene->mAnimations[1] = idle;
    fixture->idle = idle;

    // oneShot: reuses the Hip channel name (different keys), plus StaticChild.
    aiAnimation *oneShot = new aiAnimation();
    oneShot->mName = "oneShot";
    oneShot->mTicksPerSecond = 12.0;
    oneShot->mDuration = 12.0;
    oneShot->mNumChannels = 2;
    oneShot->mChannels = new aiNodeAnim *[2];
    aiNodeAnim *hipOneShot = new aiNodeAnim();
    hipOneShot->mNodeName = "Hip";
    hipOneShot->mNumPositionKeys = 2;
    hipOneShot->mPositionKeys = MakeVectorKeys(0, 2, 12.0, 0.5f, 1.0f, 0.25f, -0.3f, 0.0f, 0.2f);
    hipOneShot->mNumRotationKeys = 2;
    hipOneShot->mRotationKeys = MakeRotationKeys(0, 2, 12.0, -0.7f);
    hipOneShot->mNumScalingKeys = 2;
    hipOneShot->mScalingKeys = MakeVectorKeys(0, 2, 12.0, 1.0f, 1.0f, 1.0f, 0.02f, 0.02f, 0.02f);
    hipOneShot->mPreState = aiAnimBehaviour_LINEAR;
    hipOneShot->mPostState = aiAnimBehaviour_CONSTANT;

    aiNodeAnim *staticChildOneShot = new aiNodeAnim();
    staticChildOneShot->mNodeName = "StaticChild";
    staticChildOneShot->mNumPositionKeys = 1;
    staticChildOneShot->mPositionKeys =
            new aiVectorKey[1]{ aiVectorKey(0.0, aiVector3D(0.0f, 0.0f, 0.0f)) };
    staticChildOneShot->mNumRotationKeys = 2;
    staticChildOneShot->mRotationKeys = MakeRotationKeys(0, 2, 12.0, 1.6f);
    staticChildOneShot->mNumScalingKeys = 1;
    staticChildOneShot->mScalingKeys =
            new aiVectorKey[1]{ aiVectorKey(0.0, aiVector3D(1.0f, 1.0f, 1.0f)) };

    oneShot->mChannels[0] = hipOneShot;
    oneShot->mChannels[1] = staticChildOneShot;
    scene->mAnimations[2] = oneShot;
    fixture->oneShot = oneShot;

    return fixture;
}

// Skinning helpers: world position of a vertex with linear blend skinning.

struct BoneBind {
    aiMatrix4x4 offset;
    std::vector<std::pair<unsigned int, ai_real>> influences;
};

std::unordered_map<std::string, BoneBind> CaptureBinds(const aiMesh &mesh) {
    std::unordered_map<std::string, BoneBind> binds;
    for (unsigned int b = 0; b < mesh.mNumBones; ++b) {
        BoneBind bind;
        bind.offset = mesh.mBones[b]->mOffsetMatrix;
        for (unsigned int w = 0; w < mesh.mBones[b]->mNumWeights; ++w) {
            bind.influences.emplace_back(mesh.mBones[b]->mWeights[w].mVertexId,
                    mesh.mBones[b]->mWeights[w].mWeight);
        }
        binds.emplace(mesh.mBones[b]->mName.C_Str(), bind);
    }
    return binds;
}

// Computes a skinned world vertex given node world matrices by bone name and
// the captured bind data. Only vertices with weights are skinned; zero-weight
// vertices use the owning mesh node transform.
aiVector3D SkinVertex(const aiMesh &mesh, unsigned int vertex,
        const std::unordered_map<std::string, aiMatrix4x4> &boneWorlds,
        const std::unordered_map<std::string, BoneBind> &binds,
        const aiMatrix4x4 &meshNodeWorld) {
    aiVector3D skinned(0.0f, 0.0f, 0.0f);
    ai_real totalWeight = 0.0f;
    for (const auto &entry : binds) {
        const aiMatrix4x4 &offset = entry.second.offset;
        for (const auto &influence : entry.second.influences) {
            if (influence.first != vertex) {
                continue;
            }
            const aiMatrix4x4 skinMatrix = boneWorlds.at(entry.first) * offset;
            skinned += (skinMatrix * mesh.mVertices[vertex]) * influence.second;
            totalWeight += influence.second;
        }
    }
    if (totalWeight == 0.0f) {
        return meshNodeWorld * mesh.mVertices[vertex];
    }
    return skinned;
}

// ---------------------------------------------------------------------------
// Validation that a changed scene is structurally equivalent to the original
// and geometrically conjugated by B.
// ---------------------------------------------------------------------------

struct SceneSnapshot {
    std::vector<const aiNode *> nodePtrs;
    std::vector<std::string> nodeNames;
    std::vector<const aiMesh *> meshPtrs;
    std::vector<const aiAnimation *> animPtrs;
    std::vector<const aiBone *> bonePtrs;
    std::vector<const aiNodeAnim *> channelPtrs;
    unsigned int nodeCount = 0;
    unsigned int meshCount = 0;
};

SceneSnapshot Snapshot(const aiScene &scene) {
    SceneSnapshot s;
    CollectNodePtrs(scene.mRootNode, s.nodePtrs);
    CollectNodeNames(scene.mRootNode, s.nodeNames);
    for (unsigned int m = 0; m < scene.mNumMeshes; ++m) {
        s.meshPtrs.push_back(scene.mMeshes[m]);
        const aiMesh *mesh = scene.mMeshes[m];
        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            s.bonePtrs.push_back(mesh->mBones[b]);
        }
    }
    for (unsigned int a = 0; a < scene.mNumAnimations; ++a) {
        s.animPtrs.push_back(scene.mAnimations[a]);
        const aiAnimation *anim = scene.mAnimations[a];
        for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
            s.channelPtrs.push_back(anim->mChannels[c]);
        }
    }
    s.nodeCount = static_cast<unsigned int>(s.nodePtrs.size());
    s.meshCount = scene.mNumMeshes;
    return s;
}

bool Contains(const std::vector<const aiNode *> &v, const aiNode *p) {
    return std::find(v.begin(), v.end(), p) != v.end();
}

bool ContainsPtr(const std::vector<const aiMesh *> &v, const aiMesh *p) {
    return std::find(v.begin(), v.end(), p) != v.end();
}

bool ContainsStr(const std::vector<std::string> &v, const std::string &p) {
    return std::find(v.begin(), v.end(), p) != v.end();
}

// Verifies that every original node, mesh, animation, bone and channel object
// still exists, in the original order (semantic nodes remain siblings in the
// same relative order), names are unchanged, and that all auxiliary names are
// fresh and non-colliding.
bool PreservedStructure(const SceneSnapshot &before, const aiScene &after) {
    SceneSnapshot current = Snapshot(after);

    // All original objects survive, and no semantic object was removed.
    for (const aiNode *node : before.nodePtrs) {
        if (!Contains(current.nodePtrs, node)) {
            return false;
        }
    }
    for (const aiMesh *mesh : before.meshPtrs) {
        if (!ContainsPtr(current.meshPtrs, mesh)) {
            return false;
        }
    }
    if (current.animPtrs.size() != before.animPtrs.size()) {
        return false;
    }
    for (const aiAnimation *anim : before.animPtrs) {
        if (std::find(current.animPtrs.begin(), current.animPtrs.end(), anim) == current.animPtrs.end()) {
            return false;
        }
    }
    for (const aiBone *bone : before.bonePtrs) {
        if (std::find(current.bonePtrs.begin(), current.bonePtrs.end(), bone) == current.bonePtrs.end()) {
            return false;
        }
    }
    // Channel objects remain, but their mNodeName may have been renamed. The
    // channel order and count are unchanged.
    if (current.channelPtrs.size() != before.channelPtrs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < before.channelPtrs.size(); ++i) {
        if (current.channelPtrs[i] != before.channelPtrs[i]) {
            return false;
        }
    }

    // Original node names appear exactly once, auxiliary names never collide.
    std::unordered_map<std::string, int> nameCounts;
    for (const std::string &name : current.nodeNames) {
        ++nameCounts[name];
    }
    for (const std::string &name : before.nodeNames) {
        if (nameCounts[name] != 1) {
            return false;
        }
    }

    // Mesh references and per-node mesh lists are unchanged.
    return true;
}

// Verifies that all animation channels still hold the same key values.
bool VerifyChannelsUnchanged(const aiScene &scene,
        const std::vector<ChannelSignature> &signatures) {
    std::size_t index = 0;
    for (unsigned int a = 0; a < scene.mNumAnimations; ++a) {
        const aiAnimation *anim = scene.mAnimations[a];
        for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
            if (index >= signatures.size()) {
                return false;
            }
            if (!SameKeys(CaptureChannel(*anim->mChannels[c]), signatures[index])) {
                return false;
            }
            ++index;
        }
    }
    return index == signatures.size();
}

std::vector<ChannelSignature> CaptureAllChannels(const aiScene &scene) {
    std::vector<ChannelSignature> result;
    for (unsigned int a = 0; a < scene.mNumAnimations; ++a) {
        for (unsigned int c = 0; c < scene.mAnimations[a]->mNumChannels; ++c) {
            result.push_back(CaptureChannel(*scene.mAnimations[a]->mChannels[c]));
        }
    }
    return result;
}

// Checks the core geometric invariant for every semantic node at time t:
//   G'(t) == B * G(t) * inverse(B)
// where the semantic node is located by its original name.
bool CheckNodeWorldConjugation(const aiScene &original, const aiScene &changed,
        const aiAnimation *originalAnim, const aiAnimation *changedAnim,
        const aiMatrix4x4 &basis, double time) {
    const aiMatrix4x4 inverseBasis = Inverse(basis);
    std::vector<std::string> names;
    CollectNodeNames(original.mRootNode, names);

    for (const std::string &name : names) {
        const aiNode *before = FindUniqueNode(original.mRootNode, name.c_str());
        const aiNode *after = FindUniqueNode(changed.mRootNode, name.c_str());
        if (before == nullptr || after == nullptr) {
            return false;
        }
        const aiMatrix4x4 gBefore = SampleWorld(*before, originalAnim, time);
        const aiMatrix4x4 gAfter = SampleWorld(*after, changedAnim, time);
        if (!NearMat(gAfter, basis * gBefore * inverseBasis, static_cast<ai_real>(3e-3))) {
            return false;
        }
    }
    return true;
}

// Checks that unskinned and skinned world vertices equal (B * original).
bool CheckWorldVertices(const aiScene &original, const aiScene &changed,
        const aiAnimation *originalAnim, const aiAnimation *changedAnim,
        const aiMatrix4x4 &basis, double time, int meshIndex, const char *ownerName) {
    const aiMesh &meshBefore = *original.mMeshes[meshIndex];
    const aiMesh &meshAfter = *changed.mMeshes[meshIndex];

    const aiNode *ownerBefore = FindUniqueNode(original.mRootNode, ownerName);
    const aiNode *ownerAfter = FindUniqueNode(changed.mRootNode, ownerName);
    if (ownerBefore == nullptr || ownerAfter == nullptr) {
        return false;
    }
    const aiMatrix4x4 nodeWorldBefore = SampleWorld(*ownerBefore, originalAnim, time);
    const aiMatrix4x4 nodeWorldAfter = SampleWorld(*ownerAfter, changedAnim, time);

    const auto bindsBefore = CaptureBinds(meshBefore);
    const auto bindsAfter = CaptureBinds(meshAfter);

    std::unordered_map<std::string, aiMatrix4x4> boneWorldsBefore;
    std::unordered_map<std::string, aiMatrix4x4> boneWorldsAfter;
    for (const auto &entry : bindsBefore) {
        const aiNode *boneNodeBefore = FindUniqueNode(original.mRootNode, entry.first.c_str());
        const aiNode *boneNodeAfter = FindUniqueNode(changed.mRootNode, entry.first.c_str());
        if (boneNodeBefore == nullptr || boneNodeAfter == nullptr) {
            return false;
        }
        boneWorldsBefore.emplace(entry.first, SampleWorld(*boneNodeBefore, originalAnim, time));
        boneWorldsAfter.emplace(entry.first, SampleWorld(*boneNodeAfter, changedAnim, time));
    }

    for (unsigned int v = 0; v < meshBefore.mNumVertices; ++v) {
        // Unskinned world position.
        const aiVector3D rigidBefore = nodeWorldBefore * meshBefore.mVertices[v];
        const aiVector3D rigidAfter = nodeWorldAfter * meshAfter.mVertices[v];
        if (!NearVec(rigidAfter, basis * rigidBefore, static_cast<ai_real>(3e-3))) {
            return false;
        }

        // Linear blend skinning world position.
        const aiVector3D skinBefore = SkinVertex(meshBefore, v, boneWorldsBefore, bindsBefore,
                nodeWorldBefore);
        const aiVector3D skinAfter = SkinVertex(meshAfter, v, boneWorldsAfter, bindsAfter,
                nodeWorldAfter);
        if (!NearVec(skinAfter, basis * skinBefore, static_cast<ai_real>(3e-3))) {
            return false;
        }
    }
    return true;
}

// Verifies that a mesh's stored positions, normals, tangents and bitangents
// were transformed exactly once by B.
bool CheckMeshStreams(const aiMesh &before, const aiMesh &after, const aiMatrix4x4 &basis) {
    aiMatrix3x3 linear(basis);
    aiMatrix3x3 linearInv = linear;
    linearInv.Inverse();
    aiMatrix3x3 linearInvT = linearInv;
    linearInvT.Transpose();

    if (before.mNumVertices != after.mNumVertices) {
        return false;
    }
    for (unsigned int v = 0; v < before.mNumVertices; ++v) {
        if (!NearVec(after.mVertices[v], basis * before.mVertices[v])) {
            return false;
        }
        if (before.mNormals != nullptr && after.mNormals != nullptr) {
            const aiVector3D expected = Normalized(linearInvT * before.mNormals[v]);
            if (!NearVec(after.mNormals[v], expected)) {
                return false;
            }
        }
        if (before.mTangents != nullptr && after.mTangents != nullptr) {
            const aiVector3D expected = Normalized(linear * before.mTangents[v]);
            if (!NearVec(after.mTangents[v], expected)) {
                return false;
            }
        }
        if (before.mBitangents != nullptr && after.mBitangents != nullptr) {
            const aiVector3D expected = Normalized(linear * before.mBitangents[v]);
            if (!NearVec(after.mBitangents[v], expected)) {
                return false;
            }
        }
        // UVs and colors are unchanged.
        if (before.mTextureCoords[0] != nullptr && after.mTextureCoords[0] != nullptr) {
            if (before.mTextureCoords[0][v] != after.mTextureCoords[0][v]) {
                return false;
            }
        }
        if (before.mColors[0] != nullptr && after.mColors[0] != nullptr) {
            if (before.mColors[0][v] != after.mColors[0][v]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Structural / numeric comparison helpers used by the tests.
// ---------------------------------------------------------------------------

bool NodeMeshListEqual(const aiNode &a, const aiNode &b) {
    if (a.mNumMeshes != b.mNumMeshes) {
        return false;
    }
    for (unsigned int i = 0; i < a.mNumMeshes; ++i) {
        if (a.mMeshes[i] != b.mMeshes[i]) {
            return false;
        }
    }
    return true;
}

bool StreamEqual(const aiVector3D *a, const aiVector3D *b, unsigned int count,
        ai_real eps = static_cast<ai_real>(1e-5)) {
    if ((a == nullptr) != (b == nullptr)) {
        return false;
    }
    if (a == nullptr) {
        return true;
    }
    for (unsigned int i = 0; i < count; ++i) {
        if (!NearVec(a[i], b[i], eps)) {
            return false;
        }
    }
    return true;
}

bool MeshEquivalent(const aiMesh &a, const aiMesh &b) {
    if (a.mNumVertices != b.mNumVertices || a.mNumFaces != b.mNumFaces ||
            a.mNumBones != b.mNumBones || a.mNumAnimMeshes != b.mNumAnimMeshes) {
        return false;
    }
    if (!StreamEqual(a.mVertices, b.mVertices, a.mNumVertices)) {
        return false;
    }
    if (!StreamEqual(a.mNormals, b.mNormals, a.mNumVertices)) {
        return false;
    }
    if (!StreamEqual(a.mTangents, b.mTangents, a.mNumVertices)) {
        return false;
    }
    if (!StreamEqual(a.mBitangents, b.mBitangents, a.mNumVertices)) {
        return false;
    }
    for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
        if (!StreamEqual(a.mTextureCoords[c], b.mTextureCoords[c], a.mNumVertices)) {
            return false;
        }
    }
    for (unsigned int f = 0; f < a.mNumFaces; ++f) {
        if (a.mFaces[f].mNumIndices != b.mFaces[f].mNumIndices) {
            return false;
        }
        for (unsigned int i = 0; i < a.mFaces[f].mNumIndices; ++i) {
            if (a.mFaces[f].mIndices[i] != b.mFaces[f].mIndices[i]) {
                return false;
            }
        }
    }
    for (unsigned int bo = 0; bo < a.mNumBones; ++bo) {
        if (a.mBones[bo]->mName != b.mBones[bo]->mName) {
            return false;
        }
        if (!NearMat(a.mBones[bo]->mOffsetMatrix, b.mBones[bo]->mOffsetMatrix, static_cast<ai_real>(1e-4))) {
            return false;
        }
        if (a.mBones[bo]->mNumWeights != b.mBones[bo]->mNumWeights) {
            return false;
        }
        for (unsigned int w = 0; w < a.mBones[bo]->mNumWeights; ++w) {
            if (a.mBones[bo]->mWeights[w].mVertexId != b.mBones[bo]->mWeights[w].mVertexId ||
                    a.mBones[bo]->mWeights[w].mWeight != b.mBones[bo]->mWeights[w].mWeight) {
                return false;
            }
        }
    }
    for (unsigned int am = 0; am < a.mNumAnimMeshes; ++am) {
        const aiAnimMesh &x = *a.mAnimMeshes[am];
        const aiAnimMesh &y = *b.mAnimMeshes[am];
        if (x.mNumVertices != y.mNumVertices) {
            return false;
        }
        if (!StreamEqual(x.mVertices, y.mVertices, x.mNumVertices)) {
            return false;
        }
        if (!StreamEqual(x.mNormals, y.mNormals, x.mNumVertices)) {
            return false;
        }
        if (!StreamEqual(x.mTangents, y.mTangents, x.mNumVertices)) {
            return false;
        }
        if (!StreamEqual(x.mBitangents, y.mBitangents, x.mNumVertices)) {
            return false;
        }
    }
    return true;
}

// Compares two scenes numerically (used to prove a failed call left the scene
// untouched, and to compare two independent runs of a deterministic op).
bool ScenesEquivalent(const aiScene &a, const aiScene &b) {
    if ((a.mRootNode == nullptr) != (b.mRootNode == nullptr)) {
        return false;
    }
    if (CountNodes(a.mRootNode) != CountNodes(b.mRootNode)) {
        return false;
    }
    std::vector<std::string> namesA, namesB;
    CollectNodeNames(a.mRootNode, namesA);
    CollectNodeNames(b.mRootNode, namesB);
    std::sort(namesA.begin(), namesA.end());
    std::sort(namesB.begin(), namesB.end());
    if (namesA != namesB) {
        return false;
    }
    for (const std::string &name : namesA) {
        const aiNode *na = FindUniqueNode(a.mRootNode, name.c_str());
        const aiNode *nb = FindUniqueNode(b.mRootNode, name.c_str());
        if (na == nullptr || nb == nullptr) {
            return false;
        }
        if (!NearMat(na->mTransformation, nb->mTransformation, static_cast<ai_real>(1e-5))) {
            return false;
        }
        if (!NodeMeshListEqual(*na, *nb)) {
            return false;
        }
        if ((na->mMetaData != nullptr) != (nb->mMetaData != nullptr)) {
            return false;
        }
    }

    if (a.mNumMeshes != b.mNumMeshes || a.mNumAnimations != b.mNumAnimations) {
        return false;
    }
    for (unsigned int m = 0; m < a.mNumMeshes; ++m) {
        if (!MeshEquivalent(*a.mMeshes[m], *b.mMeshes[m])) {
            return false;
        }
    }
    for (unsigned int ai = 0; ai < a.mNumAnimations; ++ai) {
        const aiAnimation &x = *a.mAnimations[ai];
        const aiAnimation &y = *b.mAnimations[ai];
        if (x.mName != y.mName || x.mDuration != y.mDuration ||
                x.mTicksPerSecond != y.mTicksPerSecond ||
                x.mNumChannels != y.mNumChannels) {
            return false;
        }
        for (unsigned int c = 0; c < x.mNumChannels; ++c) {
            if (!SameKeys(CaptureChannel(*x.mChannels[c]), CaptureChannel(*y.mChannels[c]))) {
                return false;
            }
        }
    }
    return true;
}

// Checks that the per-node channel targets remain resolvable and unique.
bool ChannelTargetsResolvable(const aiScene &scene) {
    for (unsigned int a = 0; a < scene.mNumAnimations; ++a) {
        std::unordered_set<std::string> seenTargets;
        const aiAnimation &anim = *scene.mAnimations[a];
        for (unsigned int c = 0; c < anim.mNumChannels; ++c) {
            const std::string target = anim.mChannels[c]->mNodeName.C_Str();
            if (target.empty()) {
                return false;
            }
            if (!seenTargets.insert(target).second) {
                return false; // duplicate channel target within one animation
            }
            unsigned int count = 0;
            for (const std::string &name : [&]() {
                     std::vector<std::string> n;
                     CollectNodeNames(scene.mRootNode, n);
                     return n;
                 }()) {
                if (name == target) {
                    ++count;
                }
            }
            if (count != 1) {
                return false;
            }
        }
    }
    return true;
}

void CollectMeshOwners(const aiNode *node, unsigned int meshIndex, std::vector<std::string> &out) {
    if (node == nullptr) {
        return;
    }
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        if (node->mMeshes[i] == meshIndex) {
            out.emplace_back(node->mName.C_Str());
        }
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        CollectMeshOwners(node->mChildren[i], meshIndex, out);
    }
}

// Full geometric verification of a changed scene against a pristine original.
bool VerifyConjugatedScene(const aiScene &original, const aiScene &changed,
        const aiMatrix4x4 &basis, const std::vector<double> &sampleTimes) {
    // Static (unanimated) evaluation.
    if (!CheckNodeWorldConjugation(original, changed, nullptr, nullptr, basis, 0.0)) {
        return false;
    }

    std::vector<std::string> owners;
    CollectMeshOwners(original.mRootNode, 0, owners);
    for (const std::string &owner : owners) {
        if (!CheckWorldVertices(original, changed, nullptr, nullptr, basis, 0.0, 0,
                owner.c_str())) {
            return false;
        }
    }

    if (original.mNumAnimations != changed.mNumAnimations) {
        return false;
    }
    for (unsigned int a = 0; a < original.mNumAnimations; ++a) {
        const aiAnimation *animBefore = original.mAnimations[a];
        const aiAnimation *animAfter = changed.mAnimations[a];
        for (double time : sampleTimes) {
            if (!CheckNodeWorldConjugation(original, changed, animBefore, animAfter, basis, time)) {
                return false;
            }
            for (const std::string &owner : owners) {
                if (!CheckWorldVertices(original, changed, animBefore, animAfter, basis, time, 0,
                        owner.c_str())) {
                    return false;
                }
            }
        }
    }
    return true;
}

// A minimal animated fixture used by the collision test.
struct SmallFixture {
    aiScene *scene = nullptr;
    aiNode *root = nullptr;
    aiNode *animated = nullptr;
    ~SmallFixture() {
        delete scene;
        scene = nullptr;
    }
};

SmallFixture *BuildSmallAnimatedFixture(const std::vector<std::string> &staticNames) {
    auto *f = new SmallFixture();
    f->scene = new aiScene();
    aiScene *scene = f->scene;

    f->root = new aiNode("Root");
    scene->mRootNode = f->root;
    f->animated = new aiNode("Animated");
    f->animated->mTransformation = Translation(0.5f, 0.0f, 0.0f);

    std::vector<aiNode *> children;
    children.push_back(f->animated);
    for (const std::string &name : staticNames) {
        aiNode *n = new aiNode(name);
        n->mTransformation = Translation(1.0f, 2.0f, 3.0f);
        children.push_back(n);
    }
    f->root->mNumChildren = static_cast<unsigned int>(children.size());
    f->root->mChildren = new aiNode *[children.size()];
    for (std::size_t i = 0; i < children.size(); ++i) {
        f->root->mChildren[i] = children[i];
        children[i]->mParent = f->root;
    }
    f->root->mNumMeshes = 0;

    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh *[1]{ new aiMesh() };
    aiMesh *mesh = scene->mMeshes[0];
    mesh->mName = "Tri";
    mesh->mNumVertices = 3;
    mesh->mVertices = new aiVector3D[3]{
        aiVector3D(0.0f, 0.0f, 0.0f),
        aiVector3D(1.0f, 0.0f, 0.0f),
        aiVector3D(0.0f, 1.0f, 0.0f)
    };
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mNumFaces = 1;
    mesh->mFaces = new aiFace[1];
    mesh->mFaces[0].mNumIndices = 3;
    mesh->mFaces[0].mIndices = new unsigned int[3]{ 0, 1, 2 };
    f->animated->mNumMeshes = 1;
    f->animated->mMeshes = new unsigned int[1]{ 0 };

    scene->mNumAnimations = 1;
    scene->mAnimations = new aiAnimation *[1]{ new aiAnimation() };
    aiAnimation &anim = *scene->mAnimations[0];
    anim.mName = "spin";
    anim.mTicksPerSecond = 10.0;
    anim.mDuration = 10.0;
    anim.mNumChannels = 1;
    anim.mChannels = new aiNodeAnim *[1]{ new aiNodeAnim() };
    aiNodeAnim &channel = *anim.mChannels[0];
    channel.mNodeName = "Animated";
    channel.mNumPositionKeys = 2;
    channel.mPositionKeys = new aiVectorKey[2]{
        aiVectorKey(0.0, aiVector3D(0.5f, 0.0f, 0.0f)),
        aiVectorKey(10.0, aiVector3D(0.5f, 0.0f, 0.0f))
    };
    channel.mNumRotationKeys = 2;
    channel.mRotationKeys = new aiQuatKey[2]{
        aiQuatKey(0.0, aiQuaternion(1.0f, 0.0f, 0.0f, 0.0f)),
        aiQuatKey(10.0, aiQuaternion(aiVector3D(0.0f, 0.0f, 1.0f), 1.0f))
    };
    channel.mNumScalingKeys = 2;
    channel.mScalingKeys = new aiVectorKey[2]{
        aiVectorKey(0.0, aiVector3D(1.0f, 1.0f, 1.0f)),
        aiVectorKey(10.0, aiVector3D(1.0f, 1.0f, 1.0f))
    };
    return f;
}

// ---------------------------------------------------------------------------
// Group: geometry
// ---------------------------------------------------------------------------

int CaseGeometryMeshStreams() {
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*changed->scene, basis);

    const aiMesh &before = *original->mesh;
    const aiMesh &after = *changed->mesh;
    TASK_CHECK(CheckMeshStreams(before, after, basis));

    // Morph targets: each is transformed exactly once; absent replacement
    // streams remain absent and present ones stay present.
    TASK_CHECK(after.mNumAnimMeshes == 2);
    TASK_CHECK(after.mAnimMeshes[0]->mNumVertices == after.mNumVertices);
    TASK_CHECK(after.mAnimMeshes[1]->mNumVertices == after.mNumVertices);
    TASK_CHECK(after.mAnimMeshes[0]->mVertices != nullptr);
    TASK_CHECK(after.mAnimMeshes[0]->mNormals == nullptr);
    TASK_CHECK(after.mAnimMeshes[1]->mVertices == nullptr);
    TASK_CHECK(after.mAnimMeshes[1]->mNormals != nullptr);
    for (unsigned int v = 0; v < after.mNumVertices; ++v) {
        TASK_CHECK(NearVec(after.mAnimMeshes[0]->mVertices[v],
                basis * before.mAnimMeshes[0]->mVertices[v]));
        aiMatrix3x3 linear(basis);
        aiMatrix3x3 inv = linear;
        inv.Inverse();
        inv.Transpose();
        TASK_CHECK(NearVec(after.mAnimMeshes[1]->mNormals[v],
                Normalized(inv * before.mAnimMeshes[1]->mNormals[v])));
    }

    // Bone offsets are conjugated, not decomposed: B * O * B^-1.
    const aiMatrix4x4 inverseBasis = Inverse(basis);
    TASK_CHECK(NearMat(after.mBones[0]->mOffsetMatrix,
            basis * before.mBones[0]->mOffsetMatrix * inverseBasis,
            static_cast<ai_real>(3e-4)));
    TASK_CHECK(NearMat(after.mBones[1]->mOffsetMatrix,
            basis * before.mBones[1]->mOffsetMatrix * inverseBasis,
            static_cast<ai_real>(3e-4)));

    // Non-reflective basis keeps the face winding.
    for (unsigned int f = 0; f < after.mNumFaces; ++f) {
        TASK_CHECK(after.mFaces[f].mNumIndices == before.mFaces[f].mNumIndices);
        for (unsigned int i = 0; i < after.mFaces[f].mNumIndices; ++i) {
            TASK_CHECK(after.mFaces[f].mIndices[i] == before.mFaces[f].mIndices[i]);
        }
    }

    // Bounding box recomputed from transformed base positions.
    aiVector3D expectedMin = basis * before.mVertices[0];
    aiVector3D expectedMax = expectedMin;
    for (unsigned int v = 1; v < before.mNumVertices; ++v) {
        const aiVector3D p = basis * before.mVertices[v];
        expectedMin.x = std::min(expectedMin.x, p.x);
        expectedMin.y = std::min(expectedMin.y, p.y);
        expectedMin.z = std::min(expectedMin.z, p.z);
        expectedMax.x = std::max(expectedMax.x, p.x);
        expectedMax.y = std::max(expectedMax.y, p.y);
        expectedMax.z = std::max(expectedMax.z, p.z);
    }
    TASK_CHECK(NearVec(after.mAABB.mMin, expectedMin));
    TASK_CHECK(NearVec(after.mAABB.mMax, expectedMax));

    // Names and weights survive.
    TASK_CHECK(after.mName == before.mName);
    for (unsigned int b = 0; b < after.mNumBones; ++b) {
        TASK_CHECK(after.mBones[b]->mName == before.mBones[b]->mName);
        TASK_CHECK(after.mBones[b]->mNumWeights == before.mBones[b]->mNumWeights);
    }
    return 0;
}

int CaseGeometryReflectionWinding() {
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    const aiMatrix4x4 basis = ReflectionBasis();
    TASK_CHECK(basis.Determinant() < 0.0f);
    Assimp::ChangeSceneBasis(*changed->scene, basis);

    const aiMesh &before = *original->mesh;
    const aiMesh &after = *changed->mesh;
    TASK_CHECK(CheckMeshStreams(before, after, basis));

    // Winding order is reversed for every face.
    TASK_CHECK(after.mNumFaces == before.mNumFaces);
    for (unsigned int f = 0; f < after.mNumFaces; ++f) {
        TASK_CHECK(after.mFaces[f].mNumIndices == before.mFaces[f].mNumIndices);
        for (unsigned int i = 0; i < after.mFaces[f].mNumIndices; ++i) {
            TASK_CHECK(after.mFaces[f].mIndices[i] ==
                    before.mFaces[f].mIndices[before.mFaces[f].mNumIndices - 1 - i]);
        }
    }

    // Reversing the winding keeps the transformed geometric face normal on the
    // same side as the inverse-transpose-transformed original face normal.
    aiMatrix3x3 linear(basis);
    aiMatrix3x3 inv = linear;
    inv.Inverse();
    inv.Transpose();
    for (unsigned int f = 0; f < after.mNumFaces; ++f) {
        const aiFace &faceBefore = before.mFaces[f];
        const aiFace &faceAfter = after.mFaces[f];
        const aiVector3D &b0 = before.mVertices[faceBefore.mIndices[0]];
        const aiVector3D &b1 = before.mVertices[faceBefore.mIndices[1]];
        const aiVector3D &b2 = before.mVertices[faceBefore.mIndices[2]];
        const aiVector3D originalFaceNormal = Normalized((b1 - b0) ^ (b2 - b0));
        const aiVector3D expectedNormal = Normalized(inv * originalFaceNormal);

        const aiVector3D &a0 = after.mVertices[faceAfter.mIndices[0]];
        const aiVector3D &a1 = after.mVertices[faceAfter.mIndices[1]];
        const aiVector3D &a2 = after.mVertices[faceAfter.mIndices[2]];
        const aiVector3D transformedFaceNormal = Normalized((a1 - a0) ^ (a2 - a0));
        TASK_CHECK(transformedFaceNormal * expectedNormal > 0.0f);
    }
    return 0;
}

int CaseGeometrySharedMeshOnce() {
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*changed->scene, basis);

    // Both instance nodes still reference the same single mesh object.
    TASK_CHECK(changed->instanceA->mNumMeshes == 1 && changed->instanceA->mMeshes[0] == 0);
    TASK_CHECK(changed->instanceB->mNumMeshes == 1 && changed->instanceB->mMeshes[0] == 0);
    TASK_CHECK(changed->scene->mNumMeshes == 1);
    TASK_CHECK(changed->scene->mMeshes[0] == changed->mesh);

    // Positions were transformed by B exactly once (not B squared).
    for (unsigned int v = 0; v < changed->mesh->mNumVertices; ++v) {
        TASK_CHECK(NearVec(changed->mesh->mVertices[v],
                basis * original->mesh->mVertices[v]));
    }
    return 0;
}

int CaseGeometryIdentityNoNodes() {
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    const unsigned int before = CountNodes(changed->scene->mRootNode);
    Assimp::ChangeSceneBasis(*changed->scene, MatIdentity());
    TASK_CHECK(CountNodes(changed->scene->mRootNode) == before);
    TASK_CHECK(ScenesEquivalent(*original->scene, *changed->scene));
    return 0;
}

int CaseGeometryZeroVectors() {
    std::unique_ptr<RichFixture> fixture(BuildRichFixture());
    aiMesh *mesh = fixture->mesh;
    // Zero normal, tangent and bitangent must remain zero.
    mesh->mNormals[3] = aiVector3D(0.0f, 0.0f, 0.0f);
    mesh->mTangents[3] = aiVector3D(0.0f, 0.0f, 0.0f);
    mesh->mBitangents[3] = aiVector3D(0.0f, 0.0f, 0.0f);

    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*fixture->scene, basis);
    TASK_CHECK(mesh->mNormals[3] == aiVector3D(0.0f, 0.0f, 0.0f));
    TASK_CHECK(mesh->mTangents[3] == aiVector3D(0.0f, 0.0f, 0.0f));
    TASK_CHECK(mesh->mBitangents[3] == aiVector3D(0.0f, 0.0f, 0.0f));
    return 0;
}

int CaseGeometryTinyNonIdentityBasis() {
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    const unsigned int before = CountNodes(changed->scene->mRootNode);
    const aiMatrix4x4 basis = UniformScaledBasis(static_cast<ai_real>(1.00001));
    Assimp::ChangeSceneBasis(*changed->scene, basis);
    // A tiny scale is not the identity: it must still transform and introduce
    // the animation helper chains.
    TASK_CHECK(CountNodes(changed->scene->mRootNode) > before);
    TASK_CHECK(NearVec(changed->mesh->mVertices[3],
            basis * original->mesh->mVertices[3], static_cast<ai_real>(1e-5)));
    return 0;
}

// ---------------------------------------------------------------------------
// Group: animation
// ---------------------------------------------------------------------------

int CaseAnimationWorldConjugation() {
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*changed->scene, basis);

    // Times are deliberately off the key frames (0/12/24, 0/30/60, 0/12).
    const std::vector<double> times = { 3.0, 5.5, 17.25, 20.75, 41.5 };
    TASK_CHECK(VerifyConjugatedScene(*original->scene, *changed->scene, basis, times));
    return 0;
}

int CaseAnimationSkinnedVertices() {
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*changed->scene, basis);

    const std::vector<double> times = { 4.25, 9.5, 20.75 };
    for (unsigned int a = 0; a < original->scene->mNumAnimations; ++a) {
        for (double time : times) {
            TASK_CHECK(CheckWorldVertices(*original->scene, *changed->scene,
                    original->scene->mAnimations[a], changed->scene->mAnimations[a], basis, time,
                    0, "InstanceA"));
            TASK_CHECK(CheckWorldVertices(*original->scene, *changed->scene,
                    original->scene->mAnimations[a], changed->scene->mAnimations[a], basis, time,
                    0, "InstanceB"));
        }
    }
    return 0;
}

int CaseAnimationChannelKeysPreserved() {
    std::unique_ptr<RichFixture> fixture(BuildRichFixture());
    const std::vector<ChannelSignature> signatures = CaptureAllChannels(*fixture->scene);

    std::vector<std::pair<std::string, double>> animMeta;
    for (unsigned int a = 0; a < fixture->scene->mNumAnimations; ++a) {
        const aiAnimation *anim = fixture->scene->mAnimations[a];
        animMeta.emplace_back(anim->mName.C_Str(), anim->mDuration);
    }

    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*fixture->scene, basis);

    TASK_CHECK(VerifyChannelsUnchanged(*fixture->scene, signatures));
    for (unsigned int a = 0; a < fixture->scene->mNumAnimations; ++a) {
        const aiAnimation *anim = fixture->scene->mAnimations[a];
        TASK_CHECK(anim->mName.C_Str() == animMeta[a].first);
        TASK_CHECK(anim->mDuration == animMeta[a].second);
    }
    TASK_CHECK(fixture->scene->mAnimations[0]->mTicksPerSecond == 24.0);
    TASK_CHECK(fixture->scene->mAnimations[1]->mTicksPerSecond == 30.0);
    TASK_CHECK(fixture->scene->mAnimations[2]->mTicksPerSecond == 12.0);
    // Pre/post states unchanged.
    TASK_CHECK(fixture->scene->mAnimations[0]->mChannels[0]->mPreState == aiAnimBehaviour_REPEAT);
    TASK_CHECK(fixture->scene->mAnimations[0]->mChannels[1]->mPreState == aiAnimBehaviour_DEFAULT);
    TASK_CHECK(fixture->scene->mAnimations[0]->mChannels[1]->mPostState == aiAnimBehaviour_CONSTANT);
    TASK_CHECK(fixture->scene->mAnimations[2]->mChannels[0]->mPreState == aiAnimBehaviour_LINEAR);

    // Channels now target existing, unique nodes.
    TASK_CHECK(ChannelTargetsResolvable(*fixture->scene));
    return 0;
}

int CaseAnimationCrossAnimationReuse() {
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*changed->scene, basis);

    // The same semantic node (Hip) is driven by walk and oneShot channels and
    // has no channel in idle. All walk/oneShot Hip channels must point at the
    // same auxiliary driver node.
    const std::string walkHip = changed->walk->mChannels[0]->mNodeName.C_Str();
    const std::string oneShotHip = changed->oneShot->mChannels[0]->mNodeName.C_Str();
    TASK_CHECK(walkHip == oneShotHip);

    // idle contains only the Armature channel; no Hip channel was fabricated.
    TASK_CHECK(changed->idle->mNumChannels == 1);
    TASK_CHECK(changed->idle->mChannels[0]->mNodeName.C_Str() != walkHip);

    // Hip is not directly channel-driven in idle, yet conjugation still holds
    // at a non-key time (default-matrix behaviour).
    const double idleTime = 37.5;
    TASK_CHECK(CheckNodeWorldConjugation(*original->scene, *changed->scene,
            original->idle, changed->idle, basis, idleTime));
    TASK_CHECK(CheckWorldVertices(*original->scene, *changed->scene,
            original->idle, changed->idle, basis, idleTime, 0, "InstanceA"));
    return 0;
}

int CaseAnimationDefaultBoundaries() {
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*changed->scene, basis);

    // walk Knee uses DEFAULT before the first key and CONSTANT after the last;
    // sample well outside the key range.
    const std::vector<double> boundaryTimes = { -7.5, -0.25, 30.0, 55.5 };
    for (double time : boundaryTimes) {
        TASK_CHECK(CheckNodeWorldConjugation(*original->scene, *changed->scene,
                original->walk, changed->walk, basis, time));
        TASK_CHECK(CheckWorldVertices(*original->scene, *changed->scene,
                original->walk, changed->walk, basis, time, 0, "InstanceA"));
    }
    return 0;
}

int CaseAnimationStaticNodesConjugatedDirectly() {
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    const aiMatrix4x4 basis = ShearTranslationBasis();
    const aiMatrix4x4 inverseBasis = Inverse(basis);
    const unsigned int originalCount = CountNodes(changed->scene->mRootNode);

    Assimp::ChangeSceneBasis(*changed->scene, basis);

    // Root and StaticHelper are never channel-driven: they are conjugated in
    // place, keep their parent/child relationships and gain no helpers.
    TASK_CHECK(changed->scene->mRootNode == changed->root);
    TASK_CHECK(changed->staticHelper->mParent == changed->root);
    TASK_CHECK(changed->root->mChildren[3] == changed->staticHelper);
    TASK_CHECK(NearMat(changed->root->mTransformation,
            basis * original->root->mTransformation * inverseBasis));
    TASK_CHECK(NearMat(changed->staticHelper->mTransformation,
            basis * original->staticHelper->mTransformation * inverseBasis));

    // Exactly three auxiliary nodes per distinct animated node (Armature, Hip,
    // Knee, StaticChild).
    const unsigned int animatedNodes = 4;
    TASK_CHECK(CountNodes(changed->scene->mRootNode) == originalCount + 3 * animatedNodes);
    return 0;
}

int CaseAnimationAuxNameCollisions() {
    const std::string collisionA = "__assimp_scene_basis_aux_0";
    const std::string collisionB = "__assimp_scene_basis_aux_1";
    std::unique_ptr<SmallFixture> fixture(
            BuildSmallAnimatedFixture({ collisionA, collisionB }));

    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*fixture->scene, basis);

    std::vector<std::string> names;
    CollectNodeNames(fixture->scene->mRootNode, names);
    std::unordered_set<std::string> unique(names.begin(), names.end());
    TASK_CHECK(unique.size() == names.size()); // no duplicate names at all
    TASK_CHECK(ContainsStr(names, collisionA));
    TASK_CHECK(ContainsStr(names, collisionB));
    // The original animated node name still exists exactly once.
    TASK_CHECK(std::count(names.begin(), names.end(), std::string("Animated")) == 1);
    TASK_CHECK(ChannelTargetsResolvable(*fixture->scene));
    return 0;
}

int CaseAnimationNoMeshlessExpansionForStaticScenes() {
    // A scene without any animation must not gain auxiliary nodes.
    std::unique_ptr<SmallFixture> fixture(BuildSmallAnimatedFixture({}));
    delete fixture->scene->mAnimations[0];
    delete[] fixture->scene->mAnimations;
    fixture->scene->mAnimations = nullptr;
    fixture->scene->mNumAnimations = 0;

    const unsigned int before = CountNodes(fixture->scene->mRootNode);
    Assimp::ChangeSceneBasis(*fixture->scene, ShearTranslationBasis());
    TASK_CHECK(CountNodes(fixture->scene->mRootNode) == before);
    return 0;
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Importer / ScaleProcess integration fixtures
// ---------------------------------------------------------------------------

std::string ObjTriangle() {
    return std::string(
            "# triangle\n"
            "o Tri\n"
            "v 0 0 0\n"
            "v 1 0 0\n"
            "v 0 1 0\n"
            "vn 0 0 1\n"
            "f 1//1 2//1 3//1\n");
}

// Loads the OBJ triangle through the normal importer path (memory buffer, so no
// filesystem is required).
const aiScene *ImportTriangle(Assimp::Importer &importer, unsigned int flags) {
    const std::string obj = ObjTriangle();
    return importer.ReadFileFromMemory(obj.data(), obj.size(), flags, "obj");
}

// Finds the node that owns mesh 0 in an imported scene.
bool ReadImportedTriangle(const aiScene &scene, aiVector3D &p0, aiVector3D &p1, aiVector3D &p2) {
    if (scene.mNumMeshes != 1 || scene.mMeshes[0]->mNumVertices != 3 ||
            scene.mMeshes[0]->mNumFaces != 1) {
        return false;
    }
    p0 = scene.mMeshes[0]->mVertices[0];
    p1 = scene.mMeshes[0]->mVertices[1];
    p2 = scene.mMeshes[0]->mVertices[2];
    return true;
}

int CaseIntegrationImporterBasis() {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_GLOBAL_SCALE_BASIS_ENABLED, true);
    importer.SetPropertyMatrix(AI_CONFIG_GLOBAL_SCALE_BASIS, ShearTranslationBasis());

    const aiScene *scene = ImportTriangle(importer,
            aiProcess_Triangulate | aiProcess_GlobalScale);
    TASK_CHECK(scene != nullptr);

    aiVector3D p0, p1, p2;
    TASK_CHECK(ReadImportedTriangle(*scene, p0, p1, p2));

    // With U as the basis and s = 1, the stored positions are U * p. The OBJ
    // importer reports no file scale, and the node owning the mesh is the root
    // here (its transform is identity), so the stored positions are also the
    // world positions.
    const aiMatrix4x4 expectedBasis = ShearTranslationBasis();
    const aiNode *owner = FindMeshOwner(scene->mRootNode, 0);
    TASK_CHECK(owner != nullptr);
    TASK_CHECK(NearMat(SampleWorld(*owner, nullptr, 0.0), aiMatrix4x4()));
    TASK_CHECK(NearVec(p0, expectedBasis * aiVector3D(0.0f, 0.0f, 0.0f)));
    TASK_CHECK(NearVec(p1, expectedBasis * aiVector3D(1.0f, 0.0f, 0.0f)));
    TASK_CHECK(NearVec(p2, expectedBasis * aiVector3D(0.0f, 1.0f, 0.0f)));
    // Normals were transformed by the inverse transpose and renormalized.
    TASK_CHECK(scene->mMeshes[0]->mNormals != nullptr);
    aiMatrix3x3 linear(expectedBasis);
    aiMatrix3x3 inv = linear;
    inv.Inverse();
    inv.Transpose();
    for (unsigned int v = 0; v < 3; ++v) {
        TASK_CHECK(NearVec(scene->mMeshes[0]->mNormals[v],
                Normalized(inv * aiVector3D(0.0f, 0.0f, 1.0f))));
    }
    return 0;
}

int CaseIntegrationScaleProcessBasis() {
    // Build an in-memory scene like the OBJ triangle but with a static node
    // transform, then run ScaleProcess directly.
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    Assimp::Importer props;
    props.SetPropertyBool(AI_CONFIG_GLOBAL_SCALE_BASIS_ENABLED, true);
    props.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 2.0f);
    props.SetPropertyFloat(AI_CONFIG_APP_SCALE_KEY, 1.5f); // s = 3
    props.SetPropertyMatrix(AI_CONFIG_GLOBAL_SCALE_BASIS, ShearTranslationBasis());

    Assimp::ScaleProcess process;
    process.SetupProperties(&props);
    process.Execute(changed->scene);

    // Effective basis B = diag(s,s,s,1) * U with s = 3.
    aiMatrix4x4 scaled;
    scaled.a1 = 3.0f;
    scaled.b2 = 3.0f;
    scaled.c3 = 3.0f;
    const aiMatrix4x4 basis = scaled * ShearTranslationBasis();

    // Mesh positions became B * p.
    for (unsigned int v = 0; v < changed->mesh->mNumVertices; ++v) {
        TASK_CHECK(NearVec(changed->mesh->mVertices[v],
                basis * original->mesh->mVertices[v], static_cast<ai_real>(1e-3)));
    }
    TASK_CHECK(VerifyConjugatedScene(*original->scene, *changed->scene, basis, { 5.5, 17.25 }));
    return 0;
}

int CaseIntegrationScaleProcessDisabledLegacy() {
    // With the basis mode disabled the legacy global-scaling behaviour is
    // exactly preserved (positions, node translations and bone offsets scale,
    // no nodes are added, animations untouched in key values aside from
    // positions).
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    // The legacy ScaleProcess assumes dense morph streams; give it a dense
    // replacement so we can observe the unscaled-key behavior without relying
    // on its sparse-stream handling (which is out of scope here).
    for (RichFixture *f : { original.get(), changed.get() }) {
        for (unsigned int a = 0; a < f->mesh->mNumAnimMeshes; ++a) {
            delete f->mesh->mAnimMeshes[a];
        }
        delete[] f->mesh->mAnimMeshes;
        f->mesh->mAnimMeshes = nullptr;
        f->mesh->mNumAnimMeshes = 0;
    }

    const unsigned int nodeCount = CountNodes(changed->scene->mRootNode);

    Assimp::Importer props;
    // not enabling the basis on purpose
    props.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 2.0f);
    props.SetPropertyFloat(AI_CONFIG_APP_SCALE_KEY, 1.5f);

    Assimp::ScaleProcess process;
    process.SetupProperties(&props);
    process.Execute(changed->scene);

    TASK_CHECK(CountNodes(changed->scene->mRootNode) == nodeCount);
    for (unsigned int v = 0; v < changed->mesh->mNumVertices; ++v) {
        TASK_CHECK(NearVec(changed->mesh->mVertices[v],
                original->mesh->mVertices[v] * 3.0f));
    }
    // Node translations scale by 3 (via the legacy decompose path).
    TASK_CHECK(Near(changed->instanceA->mTransformation.a4,
            original->instanceA->mTransformation.a4 * 3.0f));
    // Animation position keys scale.
    TASK_CHECK(Near(changed->walk->mChannels[0]->mPositionKeys[1].mValue.x,
            original->walk->mChannels[0]->mPositionKeys[1].mValue.x * 3.0f));
    // Bone offset translation scales; rotation is preserved.
    TASK_CHECK(Near(changed->mesh->mBones[0]->mOffsetMatrix.a4,
            original->mesh->mBones[0]->mOffsetMatrix.a4 * 3.0f));
    return 0;
}

int CaseIntegrationImporterLegacyUnchanged() {
    // The OBJ import with plain global scale keeps its legacy geometry.
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 2.0f);
    const aiScene *scene = ImportTriangle(importer,
            aiProcess_Triangulate | aiProcess_GlobalScale);
    TASK_CHECK(scene != nullptr);
    aiVector3D p0, p1, p2;
    TASK_CHECK(ReadImportedTriangle(*scene, p0, p1, p2));
    TASK_CHECK(NearVec(p1, aiVector3D(2.0f, 0.0f, 0.0f)));
    TASK_CHECK(NearVec(p2, aiVector3D(0.0f, 2.0f, 0.0f)));
    return 0;
}

int CaseIntegrationImporterInvalidBasisRejected() {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_GLOBAL_SCALE_BASIS_ENABLED, true);
    aiMatrix4x4 singular;
    singular.a1 = 0.0f;
    singular.b2 = 0.0f;
    singular.c3 = 0.0f;
    importer.SetPropertyMatrix(AI_CONFIG_GLOBAL_SCALE_BASIS, singular);

    const aiScene *scene = ImportTriangle(importer,
            aiProcess_Triangulate | aiProcess_GlobalScale);
    // The post-process throws, the importer reports failure and returns null.
    TASK_CHECK(scene == nullptr);
    TASK_CHECK(std::strlen(importer.GetErrorString()) > 0);
    return 0;
}

int CaseIntegrationImporterInvalidScaleRejected() {
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_GLOBAL_SCALE_BASIS_ENABLED, true);
    importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 0.0f); // s = 0
    const aiScene *scene = ImportTriangle(importer,
            aiProcess_Triangulate | aiProcess_GlobalScale);
    TASK_CHECK(scene == nullptr);
    TASK_CHECK(std::strlen(importer.GetErrorString()) > 0);
    return 0;
}

int CaseIntegrationInvalidInputsPreserveScene() {
    struct InvalidCase {
        const char *label;
        void (*apply)(aiScene &);
    };

    const std::vector<InvalidCase> cases = {
        { "camera", [](aiScene &s) {
             s.mNumCameras = 1;
             s.mCameras = new aiCamera *[1]{ new aiCamera() };
         } },
        { "light", [](aiScene &s) {
             s.mNumLights = 1;
             s.mLights = new aiLight *[1]{ new aiLight() };
         } },
        { "skeleton", [](aiScene &s) {
             s.mNumSkeletons = 1;
             s.mSkeletons = new aiSkeleton *[1]{ nullptr };
         } },
        { "badBoneName", [](aiScene &s) {
             s.mMeshes[0]->mBones[0]->mName = "MissingBone";
         } },
        { "ambiguousBoneName", [](aiScene &s) {
             // Duplicate a node name so the bone resolves ambiguously.
             aiNode *dup = new aiNode("Hip");
             s.mRootNode->addChildren(1, &dup);
         } },
        { "badBoneNode", [](aiScene &s) {
             s.mMeshes[0]->mBones[0]->mNode = new aiNode("Ghost");
         } },
        { "badArmature", [](aiScene &s) {
             s.mMeshes[0]->mBones[0]->mArmature = new aiNode("Ghost");
         } },
        { "badChannelTarget", [](aiScene &s) {
             s.mAnimations[0]->mChannels[0]->mNodeName = "MissingNode";
         } },
        { "ambiguousChannelTarget", [](aiScene &s) {
             // Duplicate a channel target name so it no longer resolves
             // uniquely (oneShot targets StaticChild).
             aiNode *dup = new aiNode("StaticChild");
             s.mRootNode->addChildren(1, &dup);
         } },
        { "zeroWeightVertexId", [](aiScene &s) {
             s.mMeshes[0]->mBones[0]->mWeights[0].mVertexId = 999;
         } },
        { "nanVertex", [](aiScene &s) {
             s.mMeshes[0]->mVertices[0].x = std::numeric_limits<ai_real>::quiet_NaN();
         } },
        { "nanOffset", [](aiScene &s) {
             s.mMeshes[0]->mBones[0]->mOffsetMatrix.a1 =
                     std::numeric_limits<ai_real>::quiet_NaN();
         } },
        { "morphVertexCountMismatch", [](aiScene &s) {
             s.mMeshes[0]->mAnimMeshes[0]->mNumVertices = 3;
         } },
    };

    for (const InvalidCase &invalid : cases) {
        std::unique_ptr<RichFixture> fixture(BuildRichFixture());
        invalid.apply(*fixture->scene);

        // Snapshot the structurally observable state before the call.
        const unsigned int nodeCount = CountNodes(fixture->scene->mRootNode);
        const aiMatrix4x4 rootBefore = fixture->scene->mRootNode->mTransformation;
        const aiVector3D vertexBefore = fixture->mesh->mVertices[1];
        const aiMatrix4x4 offsetBefore = fixture->mesh->mBones[0]->mOffsetMatrix;
        const aiMatrix4x4 hipBefore = fixture->hip->mTransformation;
        const std::string hipChannelBefore = fixture->walk->mChannels[0]->mNodeName.C_Str();

        bool threw = false;
        try {
            Assimp::ChangeSceneBasis(*fixture->scene, ShearTranslationBasis());
        } catch (const DeadlyImportError &) {
            threw = true;
        }
        TASK_CHECK(threw);

        // Everything is unchanged.
        TASK_CHECK(CountNodes(fixture->scene->mRootNode) == nodeCount);
        TASK_CHECK(ExactMat(fixture->scene->mRootNode->mTransformation, rootBefore));
        TASK_CHECK(ExactVec(fixture->mesh->mVertices[1], vertexBefore));
        TASK_CHECK(ExactMat(fixture->mesh->mBones[0]->mOffsetMatrix, offsetBefore));
        TASK_CHECK(ExactMat(fixture->hip->mTransformation, hipBefore));
        TASK_CHECK(fixture->walk->mChannels[0]->mNodeName.C_Str() == hipChannelBefore);

        // And the original scene still plays.
        const aiMatrix4x4 g0 = SampleWorld(*fixture->hip, fixture->walk, 5.5);
        TASK_CHECK(std::isfinite(static_cast<double>(g0.a1)));

        // Release allocations that were made only to make the scene invalid.
        // Camera/light/skeleton arrays are owned by aiScene, but the detached
        // "ghost" nodes and the duplicated scene children are not reachable
        // from the root and would otherwise leak.
        if (std::strcmp(invalid.label, "badBoneNode") == 0 ||
                std::strcmp(invalid.label, "badArmature") == 0) {
            delete fixture->mesh->mBones[0]->mNode;
            fixture->mesh->mBones[0]->mNode = nullptr;
            delete fixture->mesh->mBones[0]->mArmature;
            fixture->mesh->mBones[0]->mArmature = nullptr;
        }
        if (std::strcmp(invalid.label, "ambiguousBoneName") == 0 ||
                std::strcmp(invalid.label, "ambiguousChannelTarget") == 0) {
            aiNode *dup = fixture->scene->mRootNode->mChildren[
                    fixture->scene->mRootNode->mNumChildren - 1];
            fixture->scene->mRootNode->mNumChildren -= 1;
            delete dup;
        }
    }
    return 0;
}

int CaseIntegrationFinalSceneStillPlays() {
    std::unique_ptr<RichFixture> fixture(BuildRichFixture());
    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*fixture->scene, basis);

    // The scene can still be sampled and evaluated everywhere.
    for (unsigned int a = 0; a < fixture->scene->mNumAnimations; ++a) {
        for (double time : { -3.0, 0.0, 7.5, 24.0, 61.25 }) {
            const aiNode *hipNode = FindUniqueNode(fixture->scene->mRootNode, "Hip");
            TASK_CHECK(hipNode != nullptr);
            const aiMatrix4x4 world = SampleWorld(*hipNode, fixture->scene->mAnimations[a], time);
            const aiVector3D p = world * aiVector3D(1.0f, 0.0f, 0.0f);
            TASK_CHECK(std::isfinite(static_cast<double>(p.x)));
            TASK_CHECK(std::isfinite(static_cast<double>(p.y)));
            TASK_CHECK(std::isfinite(static_cast<double>(p.z)));
        }
    }
    return 0;
}

int CaseIntegrationPristinePointerPreservation() {
    std::unique_ptr<RichFixture> fixture(BuildRichFixture());
    const SceneSnapshot before = Snapshot(*fixture->scene);

    // Give several nodes metadata so we can prove it survives by identity.
    fixture->hip->mMetaData = aiMetadata::Alloc(1);
    fixture->hip->mMetaData->Add("role", aiString("left-hip"));
    fixture->root->mMetaData = aiMetadata::Alloc(1);
    fixture->root->mMetaData->Add("unit", aiString("metre"));
    aiMetadata *hipMetaBefore = fixture->hip->mMetaData;
    aiMetadata *rootMetaBefore = fixture->root->mMetaData;

    // Keep the original bone node/armature pointers to verify they survive.
    aiBone *hipBone = fixture->hipBone;
    aiBone *kneeBone = fixture->kneeBone;
    aiNode *hipNode = fixture->hip;
    aiNode *kneeNode = fixture->knee;
    aiNode *armatureNode = fixture->armature;
    hipBone->mNode = hipNode;
    hipBone->mArmature = armatureNode;
    kneeBone->mNode = kneeNode;
    kneeBone->mArmature = armatureNode;

    const aiMatrix4x4 basis = ShearTranslationBasis();
    Assimp::ChangeSceneBasis(*fixture->scene, basis);

    TASK_CHECK(PreservedStructure(before, *fixture->scene));
    // Original bone mNode/mArmature pointers still point to the same semantic
    // nodes (names and identities preserved).
    TASK_CHECK(hipBone->mNode == hipNode);
    TASK_CHECK(kneeBone->mNode == kneeNode);
    TASK_CHECK(hipBone->mArmature == armatureNode);
    TASK_CHECK(kneeBone->mArmature == armatureNode);
    TASK_CHECK(FindUniqueNode(fixture->scene->mRootNode, "Hip") == hipNode);
    TASK_CHECK(FindUniqueNode(fixture->scene->mRootNode, "Knee") == kneeNode);
    TASK_CHECK(FindUniqueNode(fixture->scene->mRootNode, "Armature") == armatureNode);

    // Metadata objects survive by identity.
    TASK_CHECK(fixture->hip->mMetaData == hipMetaBefore);
    TASK_CHECK(fixture->root->mMetaData == rootMetaBefore);
    return 0;
}

int CaseIntegrationSiblingOrderAndAncestry() {
    std::unique_ptr<RichFixture> fixture(BuildRichFixture());

    // Capture the original sibling order and ancestor chain of each semantic
    // node.
    auto collectChildren = [](const aiNode *node, std::vector<const aiNode *> &out) {
        out.clear();
        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
            out.push_back(node->mChildren[i]);
        }
    };

    std::vector<const aiNode *> rootChildrenBefore, helperChildrenBefore;
    collectChildren(fixture->root, rootChildrenBefore);
    collectChildren(fixture->staticHelper, helperChildrenBefore);

    auto ancestorChain = [](const aiNode *node) {
        std::vector<const aiNode *> chain;
        for (const aiNode *p = node->mParent; p != nullptr; p = p->mParent) {
            chain.push_back(p);
        }
        return chain;
    };
    const std::vector<const aiNode *> kneeAncestorsBefore = ancestorChain(fixture->knee);
    const std::vector<const aiNode *> instanceAncestorsBefore = ancestorChain(fixture->instanceA);
    const std::vector<const aiNode *> helperAncestorsBefore = ancestorChain(fixture->staticHelper);

    // Sibling order of the semantic children is preserved even though
    // animated children are wrapped in auxiliary chains.
    Assimp::ChangeSceneBasis(*fixture->scene, ShearTranslationBasis());

    // Root's semantic children (Armature, InstanceA, InstanceB, StaticHelper)
    // appear in the same order when auxiliary wrappers are skipped.
    std::vector<const aiNode *> rootSemanticOrder;
    for (unsigned int i = 0; i < fixture->root->mNumChildren; ++i) {
        const aiNode *child = fixture->root->mChildren[i];
        // Auxiliary wrappers introduce a chain; descend to the first original
        // semantic node.
        std::unordered_set<const aiNode *> originals(rootChildrenBefore.begin(),
                rootChildrenBefore.end());
        const aiNode *semantic = child;
        while (semantic != nullptr && originals.count(semantic) == 0 &&
                semantic->mNumChildren > 0) {
            semantic = semantic->mChildren[0];
        }
        if (semantic != nullptr && originals.count(semantic) != 0) {
            rootSemanticOrder.push_back(semantic);
        }
    }
    TASK_CHECK(rootSemanticOrder.size() == rootChildrenBefore.size());
    for (std::size_t i = 0; i < rootSemanticOrder.size() && i < rootChildrenBefore.size(); ++i) {
        TASK_CHECK(rootSemanticOrder[i] == rootChildrenBefore[i]);
    }

    // StaticHelper keeps its ancestry (it is never animated), and StaticChild
    // (animated in oneShot) remains its descendant with the auxiliary wrapper
    // chain in between.
    TASK_CHECK(fixture->staticHelper->mParent == fixture->root);
    TASK_CHECK(fixture->staticHelper->mNumChildren == helperChildrenBefore.size());
    TASK_CHECK(ancestorChain(fixture->staticHelper) == helperAncestorsBefore);
    {
        std::vector<const aiNode *> staticChildAncestors = ancestorChain(fixture->staticChild);
        TASK_CHECK(std::find(staticChildAncestors.begin(), staticChildAncestors.end(),
                          fixture->staticHelper) != staticChildAncestors.end());
        TASK_CHECK(std::find(staticChildAncestors.begin(), staticChildAncestors.end(),
                          fixture->root) != staticChildAncestors.end());
    }

    // InstanceA is static: its ancestry is unchanged.
    TASK_CHECK(ancestorChain(fixture->instanceA) == instanceAncestorsBefore);

    // Knee is animated: its semantic ancestors (Hip, Armature, Root) still
    // appear in order in its ancestor chain, with auxiliary wrappers between.
    std::vector<const aiNode *> kneeAncestorsAfter = ancestorChain(fixture->knee);
    std::size_t searchFrom = 0;
    for (const aiNode *ancestor : kneeAncestorsBefore) {
        bool found = false;
        for (std::size_t i = searchFrom; i < kneeAncestorsAfter.size(); ++i) {
            if (kneeAncestorsAfter[i] == ancestor) {
                searchFrom = i + 1;
                found = true;
                break;
            }
        }
        TASK_CHECK(found);
    }
    return 0;
}

int CaseIntegrationRepeatedBasisChanges() {
    // Applying B1 then B2 equals applying B2*B1 (column vectors).
    std::unique_ptr<RichFixture> once(BuildRichFixture());
    std::unique_ptr<RichFixture> twice(BuildRichFixture());

    const aiMatrix4x4 b1 = ShearTranslationBasis();
    const aiMatrix4x4 b2 = ReflectionBasis();
    Assimp::ChangeSceneBasis(*once->scene, b2 * b1);

    Assimp::ChangeSceneBasis(*twice->scene, b1);
    Assimp::ChangeSceneBasis(*twice->scene, b2);

    const std::vector<double> times = { 5.5, 17.25 };
    for (unsigned int a = 0; a < once->scene->mNumAnimations; ++a) {
        for (double time : times) {
            // Node-world conjugation of the composed change against the
            // original fixture (the pristine original for this comparison).
            std::unique_ptr<RichFixture> original(BuildRichFixture());
            TASK_CHECK(CheckNodeWorldConjugation(*original->scene, *once->scene,
                    original->scene->mAnimations[a], once->scene->mAnimations[a],
                    b2 * b1, time));
            TASK_CHECK(CheckNodeWorldConjugation(*original->scene, *twice->scene,
                    original->scene->mAnimations[a], twice->scene->mAnimations[a],
                    b2 * b1, time));
        }
    }

    // Stored mesh positions after two changes equal B2*B1 applied once.
    std::unique_ptr<RichFixture> pristine(BuildRichFixture());
    for (unsigned int v = 0; v < once->mesh->mNumVertices; ++v) {
        const aiVector3D expected = (b2 * b1) * pristine->mesh->mVertices[v];
        TASK_CHECK(NearVec(once->mesh->mVertices[v], expected, static_cast<ai_real>(3e-3)));
        TASK_CHECK(NearVec(twice->mesh->mVertices[v], expected, static_cast<ai_real>(3e-3)));
    }
    for (unsigned int b = 0; b < once->mesh->mNumBones; ++b) {
        TASK_CHECK(NearMat(once->mesh->mBones[b]->mOffsetMatrix,
                twice->mesh->mBones[b]->mOffsetMatrix, static_cast<ai_real>(1e-3)));
    }
    return 0;
}

int CaseIntegrationIdentityViaScaleProcess() {
    // Enabling the basis mode with s = 1 and U = identity must not perturb the
    // scene.
    std::unique_ptr<RichFixture> original(BuildRichFixture());
    std::unique_ptr<RichFixture> changed(BuildRichFixture());

    Assimp::Importer props;
    props.SetPropertyBool(AI_CONFIG_GLOBAL_SCALE_BASIS_ENABLED, true);
    Assimp::ScaleProcess process;
    process.SetupProperties(&props);
    process.Execute(changed->scene);

    TASK_CHECK(ScenesEquivalent(*original->scene, *changed->scene));
    return 0;
}

// ---------------------------------------------------------------------------
// A scene that is legal without animations and with an empty morph array.
// ---------------------------------------------------------------------------

int CaseIntegrationNoAnimations() {
    std::unique_ptr<SmallFixture> fixture(BuildSmallAnimatedFixture({}));
    delete fixture->scene->mAnimations[0];
    delete[] fixture->scene->mAnimations;
    fixture->scene->mAnimations = nullptr;
    fixture->scene->mNumAnimations = 0;

    const aiMatrix4x4 basis = ShearTranslationBasis();
    const aiVector3D before = fixture->scene->mMeshes[0]->mVertices[1];
    Assimp::ChangeSceneBasis(*fixture->scene, basis);
    TASK_CHECK(NearVec(fixture->scene->mMeshes[0]->mVertices[1], basis * before));
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "geometry", "mesh_streams_transformed_once", CaseGeometryMeshStreams },
    { "geometry", "reflection_winding", CaseGeometryReflectionWinding },
    { "geometry", "shared_mesh_once", CaseGeometrySharedMeshOnce },
    { "geometry", "identity_adds_no_nodes", CaseGeometryIdentityNoNodes },
    { "geometry", "zero_vectors_stay_zero", CaseGeometryZeroVectors },
    { "geometry", "tiny_non_identity_basis", CaseGeometryTinyNonIdentityBasis },

    { "animation", "node_world_conjugation", CaseAnimationWorldConjugation },
    { "animation", "skinned_world_vertices", CaseAnimationSkinnedVertices },
    { "animation", "channel_keys_preserved", CaseAnimationChannelKeysPreserved },
    { "animation", "cross_animation_reuse", CaseAnimationCrossAnimationReuse },
    { "animation", "default_boundaries", CaseAnimationDefaultBoundaries },
    { "animation", "static_nodes_conjugated_directly", CaseAnimationStaticNodesConjugatedDirectly },
    { "animation", "aux_name_collisions", CaseAnimationAuxNameCollisions },
    { "animation", "no_meshless_expansion", CaseAnimationNoMeshlessExpansionForStaticScenes },

    { "integration", "importer_basis", CaseIntegrationImporterBasis },
    { "integration", "scale_process_basis", CaseIntegrationScaleProcessBasis },
    { "integration", "scale_process_disabled_legacy", CaseIntegrationScaleProcessDisabledLegacy },
    { "integration", "importer_legacy_unchanged", CaseIntegrationImporterLegacyUnchanged },
    { "integration", "importer_invalid_basis_rejected", CaseIntegrationImporterInvalidBasisRejected },
    { "integration", "importer_invalid_scale_rejected", CaseIntegrationImporterInvalidScaleRejected },
    { "integration", "invalid_inputs_preserve_scene", CaseIntegrationInvalidInputsPreserveScene },
    { "integration", "final_scene_still_plays", CaseIntegrationFinalSceneStillPlays },
    { "integration", "pristine_pointer_preservation", CaseIntegrationPristinePointerPreservation },
    { "integration", "sibling_order_and_ancestry", CaseIntegrationSiblingOrderAndAncestry },
    { "integration", "repeated_basis_changes", CaseIntegrationRepeatedBasisChanges },
    { "integration", "identity_via_scale_process", CaseIntegrationIdentityViaScaleProcess },
    { "integration", "scene_without_animations", CaseIntegrationNoAnimations },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
