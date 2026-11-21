/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
copyright notice, this list of conditions and the
following disclaimer.

* Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the
following disclaimer in the documentation and/or other
materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
contributors may be used to endorse or promote products
derived from this software without specific prior
written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

/** @file  SceneBasisProcess.cpp
 *  @brief Implementation of Assimp::ChangeSceneBasis().
 */

#include "SceneBasisProcess.h"

#include <assimp/Exceptional.h>
#include <assimp/anim.h>
#include <assimp/mesh.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Assimp {
namespace {

//! Prefix used for every auxiliary node we introduce. The numeric suffix is
//! chosen at run time so that we never collide with an existing node name.
constexpr const char *AuxNamePrefix = "__assimp_scene_basis_aux_";

bool IsFiniteValue(ai_real value) {
    return std::isfinite(static_cast<double>(value));
}

bool IsFiniteVector(const aiVector3D &v) {
    return IsFiniteValue(v.x) && IsFiniteValue(v.y) && IsFiniteValue(v.z);
}

bool IsFiniteQuaternion(const aiQuaternion &q) {
    return IsFiniteValue(q.w) && IsFiniteValue(q.x) && IsFiniteValue(q.y) && IsFiniteValue(q.z);
}

bool IsFiniteMatrix(const aiMatrix4x4 &m) {
    const ai_real values[16] = {
        m.a1, m.a2, m.a3, m.a4,
        m.b1, m.b2, m.b3, m.b4,
        m.c1, m.c2, m.c3, m.c4,
        m.d1, m.d2, m.d3, m.d4
    };
    for (ai_real value : values) {
        if (!IsFiniteValue(value)) {
            return false;
        }
    }
    return true;
}

//! Exact identity test: the change of basis is skipped (and no auxiliary nodes
//! are introduced) only for the identity basis.
bool IsExactIdentity(const aiMatrix4x4 &m) {
    return m.a1 == static_cast<ai_real>(1) && m.a2 == static_cast<ai_real>(0) &&
           m.a3 == static_cast<ai_real>(0) && m.a4 == static_cast<ai_real>(0) &&
           m.b1 == static_cast<ai_real>(0) && m.b2 == static_cast<ai_real>(1) &&
           m.b3 == static_cast<ai_real>(0) && m.b4 == static_cast<ai_real>(0) &&
           m.c1 == static_cast<ai_real>(0) && m.c2 == static_cast<ai_real>(0) &&
           m.c3 == static_cast<ai_real>(1) && m.c4 == static_cast<ai_real>(0) &&
           m.d1 == static_cast<ai_real>(0) && m.d2 == static_cast<ai_real>(0) &&
           m.d3 == static_cast<ai_real>(0) && m.d4 == static_cast<ai_real>(1);
}

//! Validate the basis and compute its inverse. The matrix has to be a finite,
//! invertible affine transform (last row 0,0,0,1 with column vectors).
bool BuildInverseAffine(const aiMatrix4x4 &basis, aiMatrix4x4 &outInverse) {
    if (!IsFiniteMatrix(basis)) {
        return false;
    }
    if (basis.d1 != static_cast<ai_real>(0) || basis.d2 != static_cast<ai_real>(0) ||
            basis.d3 != static_cast<ai_real>(0) || basis.d4 != static_cast<ai_real>(1)) {
        return false;
    }

    const ai_real det = basis.Determinant();
    if (!IsFiniteValue(det) || det == static_cast<ai_real>(0)) {
        return false;
    }

    aiMatrix4x4 inverse = basis;
    inverse.Inverse();
    if (!IsFiniteMatrix(inverse)) {
        return false;
    }

    outInverse = inverse;
    return true;
}

struct NodeRecord {
    aiNode *node = nullptr;
    aiNode *parent = nullptr;
    unsigned int slot = 0; //!< index in parent->mChildren, or 0 for the root
};

struct SceneModel {
    std::vector<NodeRecord> nodes;
    std::unordered_map<const aiNode *, std::size_t> nodeIndex;
    std::unordered_map<std::string, std::vector<aiNode *>> nodesByName;
    std::unordered_set<aiNode *> animatedNodes;
    std::unordered_map<const aiNode *, aiNode *> animatedRoots;
};

bool CollectNodes(aiNode *node, aiNode *parent, unsigned int slot, std::size_t depth,
        SceneModel &model, std::size_t &nodeCount) {
    if (node == nullptr) {
        return false;
    }
    // The input is expected to be a tree. Guard against cycles/shared nodes and
    // pathological depth without relying on the caller.
    if (depth > 1024 || model.nodeIndex.count(node) != 0) {
        return false;
    }
    if (nodeCount > 1000000) {
        return false;
    }

    const std::size_t index = model.nodes.size();
    model.nodeIndex.emplace(node, index);
    model.nodes.push_back(NodeRecord{ node, parent, slot });
    ++nodeCount;

    model.nodesByName[std::string(node->mName.C_Str())].push_back(node);

    if (node->mNumChildren > 0 && node->mChildren == nullptr) {
        return false;
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        if (!CollectNodes(node->mChildren[i], node, i, depth + 1, model, nodeCount)) {
            return false;
        }
    }
    return true;
}

bool ValidateMeshStreams(const aiMesh *mesh) {
    if (mesh == nullptr) {
        return false;
    }
    if (mesh->mNumVertices > 0 && mesh->mVertices == nullptr) {
        return false;
    }
    for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
        if (!IsFiniteVector(mesh->mVertices[v])) {
            return false;
        }
        if (mesh->mNormals != nullptr && !IsFiniteVector(mesh->mNormals[v])) {
            return false;
        }
        if (mesh->mTangents != nullptr && !IsFiniteVector(mesh->mTangents[v])) {
            return false;
        }
        if (mesh->mBitangents != nullptr && !IsFiniteVector(mesh->mBitangents[v])) {
            return false;
        }
        for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_COLOR_SETS; ++c) {
            if (mesh->mColors[c] != nullptr) {
                const aiColor4D &col = mesh->mColors[c][v];
                if (!IsFiniteValue(col.r) || !IsFiniteValue(col.g) ||
                        !IsFiniteValue(col.b) || !IsFiniteValue(col.a)) {
                    return false;
                }
            }
        }
        for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
            if (mesh->mTextureCoords[c] != nullptr &&
                    !IsFiniteVector(mesh->mTextureCoords[c][v])) {
                return false;
            }
        }
    }

    if (mesh->mNumFaces > 0 && mesh->mFaces == nullptr) {
        return false;
    }
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices > 0 && face.mIndices == nullptr) {
            return false;
        }
        for (unsigned int i = 0; i < face.mNumIndices; ++i) {
            if (face.mIndices[i] >= mesh->mNumVertices) {
                return false;
            }
        }
    }

    return true;
}

bool ValidateAnimMeshStreams(const aiMesh *mesh, const aiAnimMesh *animMesh) {
    if (animMesh == nullptr) {
        return false;
    }
    // A morph target is an absolute replacement stream: it has exactly as many
    // vertices as the base mesh.
    if (animMesh->mNumVertices != mesh->mNumVertices) {
        return false;
    }

    const unsigned int count = animMesh->mNumVertices;
    for (unsigned int v = 0; v < count; ++v) {
        if (animMesh->mVertices != nullptr && !IsFiniteVector(animMesh->mVertices[v])) {
            return false;
        }
        if (animMesh->mNormals != nullptr && !IsFiniteVector(animMesh->mNormals[v])) {
            return false;
        }
        if (animMesh->mTangents != nullptr && !IsFiniteVector(animMesh->mTangents[v])) {
            return false;
        }
        if (animMesh->mBitangents != nullptr && !IsFiniteVector(animMesh->mBitangents[v])) {
            return false;
        }
        for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_COLOR_SETS; ++c) {
            if (animMesh->mColors[c] != nullptr) {
                const aiColor4D &col = animMesh->mColors[c][v];
                if (!IsFiniteValue(col.r) || !IsFiniteValue(col.g) ||
                        !IsFiniteValue(col.b) || !IsFiniteValue(col.a)) {
                    return false;
                }
            }
        }
        for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
            if (animMesh->mTextureCoords[c] != nullptr &&
                    !IsFiniteVector(animMesh->mTextureCoords[c][v])) {
                return false;
            }
        }
    }
    return true;
}

bool ValidateScene(const aiScene &scene, SceneModel &model) {
    if (scene.mRootNode == nullptr) {
        return false;
    }
    // Cameras, lights and the aiSkeleton array are explicitly unsupported.
    if (scene.mNumCameras > 0 || scene.mNumLights > 0 || scene.mNumSkeletons > 0) {
        return false;
    }

    std::size_t nodeCount = 0;
    if (!CollectNodes(scene.mRootNode, nullptr, 0, 0, model, nodeCount)) {
        return false;
    }

    for (const NodeRecord &record : model.nodes) {
        if (!IsFiniteMatrix(record.node->mTransformation)) {
            return false;
        }
    }

    // Validate meshes and bones.
    if (scene.mNumMeshes > 0 && scene.mMeshes == nullptr) {
        return false;
    }
    std::unordered_set<const aiMesh *> seenMeshes;
    for (unsigned int m = 0; m < scene.mNumMeshes; ++m) {
        if (scene.mMeshes == nullptr || scene.mMeshes[m] == nullptr) {
            return false;
        }
        const aiMesh *mesh = scene.mMeshes[m];
        if (!seenMeshes.insert(mesh).second) {
            continue;
        }
        if (!ValidateMeshStreams(mesh)) {
            return false;
        }

        if (mesh->mNumAnimMeshes > 0 && mesh->mAnimMeshes == nullptr) {
            return false;
        }
        for (unsigned int a = 0; a < mesh->mNumAnimMeshes; ++a) {
            if (!ValidateAnimMeshStreams(mesh, mesh->mAnimMeshes[a])) {
                return false;
            }
        }

        if (mesh->mNumBones > 0 && mesh->mBones == nullptr) {
            return false;
        }
        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            const aiBone *bone = mesh->mBones[b];
            if (bone == nullptr) {
                return false;
            }
            if (!IsFiniteMatrix(bone->mOffsetMatrix)) {
                return false;
            }
            if (bone->mNumWeights > 0 && bone->mWeights == nullptr) {
                return false;
            }
            for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
                if (bone->mWeights[w].mVertexId >= mesh->mNumVertices) {
                    return false;
                }
                if (!IsFiniteValue(bone->mWeights[w].mWeight)) {
                    return false;
                }
            }

            // The bone name must resolve to exactly one node, and a non-null
            // mNode has to designate that same node.
            const auto named = model.nodesByName.find(std::string(bone->mName.C_Str()));
            if (named == model.nodesByName.end() || named->second.size() != 1) {
                return false;
            }
            aiNode *boneNode = named->second.front();
#ifndef ASSIMP_BUILD_NO_ARMATUREPOPULATE_PROCESS
            if (bone->mNode != nullptr && bone->mNode != boneNode) {
                return false;
            }
            // A non-null armature has to be a node inside the scene tree.
            if (bone->mArmature != nullptr && model.nodeIndex.count(bone->mArmature) == 0) {
                return false;
            }
#endif
            (void)boneNode;
        }
    }

    // Validate animations and gather the set of nodes that are channel-driven.
    if (scene.mNumAnimations > 0 && scene.mAnimations == nullptr) {
        return false;
    }
    for (unsigned int a = 0; a < scene.mNumAnimations; ++a) {
        const aiAnimation *anim = scene.mAnimations[a];
        if (anim == nullptr) {
            return false;
        }
        if (anim->mNumChannels > 0 && anim->mChannels == nullptr) {
            return false;
        }
        for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim *channel = anim->mChannels[c];
            if (channel == nullptr) {
                return false;
            }

            // Every animation target must exist and be unique.
            const auto named = model.nodesByName.find(std::string(channel->mNodeName.C_Str()));
            if (named == model.nodesByName.end() || named->second.size() != 1) {
                return false;
            }
            model.animatedNodes.insert(named->second.front());

            if (channel->mNumPositionKeys > 0 && channel->mPositionKeys == nullptr) {
                return false;
            }
            for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k) {
                if (!IsFiniteValue(channel->mPositionKeys[k].mTime) ||
                        !IsFiniteVector(channel->mPositionKeys[k].mValue)) {
                    return false;
                }
            }
            if (channel->mNumRotationKeys > 0 && channel->mRotationKeys == nullptr) {
                return false;
            }
            for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k) {
                if (!IsFiniteValue(channel->mRotationKeys[k].mTime) ||
                        !IsFiniteQuaternion(channel->mRotationKeys[k].mValue)) {
                    return false;
                }
            }
            if (channel->mNumScalingKeys > 0 && channel->mScalingKeys == nullptr) {
                return false;
            }
            for (unsigned int k = 0; k < channel->mNumScalingKeys; ++k) {
                if (!IsFiniteValue(channel->mScalingKeys[k].mTime) ||
                        !IsFiniteVector(channel->mScalingKeys[k].mValue)) {
                    return false;
                }
            }
        }
    }

    return true;
}

aiVector3D TransformPoint(const aiMatrix4x4 &m, const aiVector3D &p) {
    return m * p;
}

aiVector3D TransformNormalizedDirection(const aiMatrix3x3 &m, const aiVector3D &v) {
    if (v.SquareLength() == static_cast<ai_real>(0)) {
        return v;
    }
    aiVector3D result = m * v;
    result.NormalizeSafe();
    return result;
}

void TransformMeshStreams(aiMesh *mesh, const aiMatrix4x4 &basis,
        const aiMatrix3x3 &linear, const aiMatrix3x3 &linearInverseTranspose,
        bool reflected) {
    for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
        mesh->mVertices[v] = TransformPoint(basis, mesh->mVertices[v]);
        if (mesh->mNormals != nullptr) {
            mesh->mNormals[v] = TransformNormalizedDirection(linearInverseTranspose, mesh->mNormals[v]);
        }
        if (mesh->mTangents != nullptr) {
            mesh->mTangents[v] = TransformNormalizedDirection(linear, mesh->mTangents[v]);
        }
        if (mesh->mBitangents != nullptr) {
            mesh->mBitangents[v] = TransformNormalizedDirection(linear, mesh->mBitangents[v]);
        }
        // Texture coordinates and vertex colors are invariant under a change of
        // basis and are intentionally left untouched.
    }

    if (reflected) {
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            aiFace &face = mesh->mFaces[f];
            std::reverse(face.mIndices, face.mIndices + face.mNumIndices);
        }
    }

    // Recompute the bounding box from the transformed base positions.
    if (mesh->mNumVertices > 0) {
        aiVector3D minimum = mesh->mVertices[0];
        aiVector3D maximum = mesh->mVertices[0];
        for (unsigned int v = 1; v < mesh->mNumVertices; ++v) {
            const aiVector3D &p = mesh->mVertices[v];
            minimum.x = std::min(minimum.x, p.x);
            minimum.y = std::min(minimum.y, p.y);
            minimum.z = std::min(minimum.z, p.z);
            maximum.x = std::max(maximum.x, p.x);
            maximum.y = std::max(maximum.y, p.y);
            maximum.z = std::max(maximum.z, p.z);
        }
        mesh->mAABB.mMin = minimum;
        mesh->mAABB.mMax = maximum;
    }
}

void TransformAnimMeshStreams(aiAnimMesh *animMesh, const aiMatrix4x4 &basis,
        const aiMatrix3x3 &linear, const aiMatrix3x3 &linearInverseTranspose) {
    for (unsigned int v = 0; v < animMesh->mNumVertices; ++v) {
        if (animMesh->mVertices != nullptr) {
            animMesh->mVertices[v] = TransformPoint(basis, animMesh->mVertices[v]);
        }
        if (animMesh->mNormals != nullptr) {
            animMesh->mNormals[v] = TransformNormalizedDirection(linearInverseTranspose, animMesh->mNormals[v]);
        }
        if (animMesh->mTangents != nullptr) {
            animMesh->mTangents[v] = TransformNormalizedDirection(linear, animMesh->mTangents[v]);
        }
        if (animMesh->mBitangents != nullptr) {
            animMesh->mBitangents[v] = TransformNormalizedDirection(linear, animMesh->mBitangents[v]);
        }
        // Morph UV/color replacement streams are invariant and left untouched.
        // Any stream that was absent remains absent.
    }
}

void TransformMeshes(aiScene &scene, const aiMatrix4x4 &basis, const aiMatrix4x4 &inverseBasis) {
    aiMatrix3x3 linear(basis);
    aiMatrix3x3 linearInverse = linear;
    linearInverse.Inverse();
    aiMatrix3x3 linearInverseTranspose = linearInverse;
    linearInverseTranspose.Transpose();

    const bool reflected = linear.Determinant() < static_cast<ai_real>(0);

    std::unordered_set<const aiMesh *> processed;
    for (unsigned int m = 0; m < scene.mNumMeshes; ++m) {
        aiMesh *mesh = scene.mMeshes[m];
        // A mesh is a stored object and is transformed exactly once, no matter
        // how many nodes reference it.
        if (!processed.insert(mesh).second) {
            continue;
        }

        TransformMeshStreams(mesh, basis, linear, linearInverseTranspose, reflected);

        for (unsigned int a = 0; a < mesh->mNumAnimMeshes; ++a) {
            TransformAnimMeshStreams(mesh->mAnimMeshes[a], basis, linear, linearInverseTranspose);
        }

        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            aiBone *bone = mesh->mBones[b];
            // Conjugate the inverse bind matrix directly, without decomposing it.
            bone->mOffsetMatrix = basis * bone->mOffsetMatrix * inverseBasis;
        }
    }
}

void BuildNodeAuxiliaries(aiScene &scene, SceneModel &model,
        const aiMatrix4x4 &basis, const aiMatrix4x4 &inverseBasis) {
    if (model.animatedNodes.empty()) {
        return;
    }

    std::unordered_set<std::string> usedNames;
    usedNames.reserve(model.nodes.size());
    for (const NodeRecord &record : model.nodes) {
        usedNames.insert(std::string(record.node->mName.C_Str()));
    }

    unsigned int nextAuxId = 0;
    const auto makeAuxName = [&]() {
        for (;;) {
            std::string candidate = std::string(AuxNamePrefix) + std::to_string(nextAuxId++);
            if (usedNames.insert(candidate).second) {
                return candidate;
            }
        }
    };

    for (const NodeRecord &record : model.nodes) {
        aiNode *node = record.node;
        if (model.animatedNodes.count(node) == 0) {
            continue;
        }

        aiNode *pre = new aiNode(makeAuxName());
        aiNode *driver = new aiNode(makeAuxName());
        aiNode *post = new aiNode(makeAuxName());

        // The chain parent -> pre(B) -> driver(original TRS + channels) ->
        // post(B^-1) -> node(identity) makes the semantic node's global matrix
        // exactly B * G(t) * B^-1 while the animation key values stay untouched.
        pre->mTransformation = basis;
        driver->mTransformation = node->mTransformation;
        post->mTransformation = inverseBasis;

        pre->mNumChildren = 1;
        pre->mChildren = new aiNode *[1]{ driver };
        driver->mParent = pre;

        driver->mNumChildren = 1;
        driver->mChildren = new aiNode *[1]{ post };
        post->mParent = driver;

        post->mNumChildren = 1;
        post->mChildren = new aiNode *[1]{ node };
        node->mParent = post;

        // The semantic node itself no longer carries a local transform.
        node->mTransformation = aiMatrix4x4();

        if (record.parent != nullptr) {
            pre->mParent = record.parent;
            record.parent->mChildren[record.slot] = pre;
        } else {
            pre->mParent = nullptr;
            scene.mRootNode = pre;
        }

        // The semantic node has to be found by name through the auxiliary
        // driver that carries the channels.
        model.animatedRoots.emplace(node, driver);
    }
}

void ConjugateStaticNodes(SceneModel &model, const aiMatrix4x4 &basis, const aiMatrix4x4 &inverseBasis) {
    for (const NodeRecord &record : model.nodes) {
        aiNode *node = record.node;
        if (model.animatedNodes.count(node) != 0) {
            continue;
        }
        node->mTransformation = basis * node->mTransformation * inverseBasis;
    }
}

void RenameAnimationChannels(aiScene &scene, const SceneModel &model) {
    for (unsigned int a = 0; a < scene.mNumAnimations; ++a) {
        aiAnimation *anim = scene.mAnimations[a];
        for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
            aiNodeAnim *channel = anim->mChannels[c];
            const auto named = model.nodesByName.find(std::string(channel->mNodeName.C_Str()));
            if (named == model.nodesByName.end() || named->second.size() != 1) {
                continue;
            }
            const auto helper = model.animatedRoots.find(named->second.front());
            if (helper != model.animatedRoots.end()) {
                channel->mNodeName = helper->second->mName;
            }
        }
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
void ChangeSceneBasis(aiScene &scene, const aiMatrix4x4 &basis) {
    aiMatrix4x4 inverseBasis;
    if (!BuildInverseAffine(basis, inverseBasis)) {
        throw DeadlyImportError("ChangeSceneBasis: the basis matrix must be a finite, invertible affine transform.");
    }

    SceneModel model;
    if (!ValidateScene(scene, model)) {
        throw DeadlyImportError("ChangeSceneBasis: the scene is not supported or contains invalid data.");
    }

    if (IsExactIdentity(basis)) {
        // Nothing to do and, importantly, no auxiliary nodes are introduced.
        return;
    }

    TransformMeshes(scene, basis, inverseBasis);
    BuildNodeAuxiliaries(scene, model, basis, inverseBasis);
    ConjugateStaticNodes(model, basis, inverseBasis);
    RenameAnimationChannels(scene, model);
}

} // namespace Assimp
