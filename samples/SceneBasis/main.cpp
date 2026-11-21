// -----------------------------------------------------------------------------
// SceneBasis sample
//
// Programmatically builds a small animated scene whose single mesh is shared by
// two instance nodes, runs the production ChangeSceneBasis() entry point and
// prints the actual world-space vertex positions before and after the change for
// two times that are not keyframes.
// -----------------------------------------------------------------------------

#include "PostProcessing/SceneBasisProcess.h"

#include <assimp/anim.h>
#include <assimp/mesh.h>
#include <assimp/scene.h>
#include <assimp/types.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

constexpr double kTicksPerSecond = 24.0;

// --- Minimal independent animation sampler ----------------------------------
// Samples a single TRS track (position or scaling) honouring DEFAULT pre/post
// behaviour with simple linear interpolation between adjacent keys.
aiVector3D SampleVectorTrack(const aiVectorKey *keys, unsigned int count,
        const aiVector3D &defaultValue, aiAnimBehaviour pre, aiAnimBehaviour post, double time) {
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
            const double factor = span > 0.0 ? (time - keys[i - 1].mTime) / span : 0.0;
            return keys[i - 1].mValue * static_cast<ai_real>(1.0 - factor) +
                   keys[i].mValue * static_cast<ai_real>(factor);
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
            const double factor = span > 0.0 ? (time - keys[i - 1].mTime) / span : 0.0;
            aiQuaternion result;
            aiQuaternion::Interpolate(result, keys[i - 1].mValue, keys[i].mValue,
                    static_cast<ai_real>(factor));
            result.Normalize();
            return result;
        }
    }
    return keys[count - 1].mValue;
}

// The local transform of a node at a given animation time.
aiMatrix4x4 SampleLocal(const aiNode &node, const aiAnimation *animation, double time) {
    if (animation != nullptr) {
        for (unsigned int c = 0; c < animation->mNumChannels; ++c) {
            const aiNodeAnim *channel = animation->mChannels[c];
            if (channel->mNodeName != node.mName) {
                continue;
            }
            aiVector3D defaultScale, defaultPosition;
            aiQuaternion defaultRotation;
            node.mTransformation.Decompose(defaultScale, defaultRotation, defaultPosition);

            const aiVector3D position = SampleVectorTrack(channel->mPositionKeys,
                    channel->mNumPositionKeys, defaultPosition, channel->mPreState,
                    channel->mPostState, time);
            const aiQuaternion rotation = SampleQuatTrack(channel->mRotationKeys,
                    channel->mNumRotationKeys, defaultRotation, channel->mPreState,
                    channel->mPostState, time);
            const aiVector3D scaling = SampleVectorTrack(channel->mScalingKeys,
                    channel->mNumScalingKeys, defaultScale, channel->mPreState,
                    channel->mPostState, time);
            return aiMatrix4x4(scaling, rotation, position);
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

const aiNode *FindNodeByName(const aiNode *node, const char *name) {
    if (node == nullptr) {
        return nullptr;
    }
    if (std::string(node->mName.C_Str()) == name) {
        return node;
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        if (const aiNode *found = FindNodeByName(node->mChildren[i], name)) {
            return found;
        }
    }
    return nullptr;
}

// Finds the node that references the given mesh index.
const aiNode *FindMeshOwner(const aiNode *node, unsigned int meshIndex) {
    if (node == nullptr) {
        return nullptr;
    }
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        if (node->mMeshes[i] == meshIndex) {
            return node;
        }
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        if (const aiNode *found = FindMeshOwner(node->mChildren[i], meshIndex)) {
            return found;
        }
    }
    return nullptr;
}

void PrintInstance(const char *label, double time, const aiScene &scene,
        const char *instanceName, unsigned int meshIndex) {
    const aiNode *owner = FindNodeByName(scene.mRootNode, instanceName);
    const aiMesh &mesh = *scene.mMeshes[meshIndex];
    const aiAnimation *animation = scene.mNumAnimations > 0 ? scene.mAnimations[0] : nullptr;
    const aiMatrix4x4 world = SampleWorld(*owner, animation, time);
    const aiVector3D v0 = world * mesh.mVertices[0];
    const aiVector3D v1 = world * mesh.mVertices[1];
    std::printf("%s t=%6.2f %-10s v0=(%8.4f %8.4f %8.4f) v1=(%8.4f %8.4f %8.4f)\n",
            label, time, instanceName, v0.x, v0.y, v0.z, v1.x, v1.y, v1.z);
}

} // namespace

int main() {
    aiScene scene;
    scene.mRootNode = new aiNode("Root");

    aiNode *pivot = new aiNode("Pivot");
    aiNode *instanceA = new aiNode("InstanceA");
    aiNode *instanceB = new aiNode("InstanceB");

    pivot->mTransformation = aiMatrix4x4();
    pivot->mTransformation.a4 = 1.0f;
    instanceA->mTransformation.b4 = 0.75f;
    instanceB->mTransformation.b4 = -0.75f;

    // A single mesh, shared by the two instance nodes.
    scene.mNumMeshes = 1;
    scene.mMeshes = new aiMesh *[1]{ new aiMesh() };
    aiMesh &mesh = *scene.mMeshes[0];
    mesh.mName = "SharedMesh";
    mesh.mNumVertices = 3;
    mesh.mVertices = new aiVector3D[3]{
        aiVector3D(0.0f, 0.0f, 0.0f),
        aiVector3D(1.0f, 0.0f, 0.0f),
        aiVector3D(0.0f, 1.0f, 0.0f)
    };
    mesh.mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh.mNumFaces = 1;
    mesh.mFaces = new aiFace[1];
    mesh.mFaces[0].mNumIndices = 3;
    mesh.mFaces[0].mIndices = new unsigned int[3]{ 0, 1, 2 };

    instanceA->mNumMeshes = 1;
    instanceA->mMeshes = new unsigned int[1]{ 0 };
    instanceB->mNumMeshes = 1;
    instanceB->mMeshes = new unsigned int[1]{ 0 };

    pivot->mNumChildren = 2;
    pivot->mChildren = new aiNode *[2]{ instanceA, instanceB };
    instanceA->mParent = pivot;
    instanceB->mParent = pivot;

    scene.mRootNode->mNumChildren = 1;
    scene.mRootNode->mChildren = new aiNode *[1]{ pivot };
    pivot->mParent = scene.mRootNode;

    // A continuous rotation of the pivot.
    scene.mNumAnimations = 1;
    scene.mAnimations = new aiAnimation *[1]{ new aiAnimation() };
    aiAnimation &animation = *scene.mAnimations[0];
    animation.mName = "Spin";
    animation.mTicksPerSecond = kTicksPerSecond;
    animation.mDuration = kTicksPerSecond;
    animation.mNumChannels = 1;
    animation.mChannels = new aiNodeAnim *[1]{ new aiNodeAnim() };
    aiNodeAnim &channel = *animation.mChannels[0];
    channel.mNodeName = "Pivot";
    channel.mNumPositionKeys = 2;
    channel.mPositionKeys = new aiVectorKey[2]{
        aiVectorKey(0.0, aiVector3D(1.0f, 0.0f, 0.0f)),
        aiVectorKey(kTicksPerSecond, aiVector3D(1.0f, 0.0f, 0.0f))
    };
    channel.mNumRotationKeys = 2;
    channel.mRotationKeys = new aiQuatKey[2]{
        aiQuatKey(0.0, aiQuaternion(1.0f, 0.0f, 0.0f, 0.0f)),
        aiQuatKey(kTicksPerSecond, aiQuaternion(0.70710678f, 0.0f, 0.0f, 0.70710678f))
    };
    channel.mNumScalingKeys = 2;
    channel.mScalingKeys = new aiVectorKey[2]{
        aiVectorKey(0.0, aiVector3D(1.0f, 1.0f, 1.0f)),
        aiVectorKey(kTicksPerSecond, aiVector3D(1.0f, 1.0f, 1.0f))
    };

    // Basis with translation, non-uniform scale and shear.
    aiMatrix4x4 basis;
    basis.a1 = 1.25f;
    basis.a2 = 0.35f;
    basis.a4 = 2.0f;
    basis.b2 = 0.8f;
    basis.b4 = -1.5f;
    basis.c3 = 1.1f;
    basis.c4 = 0.25f;

    const double sampleTimes[2] = { 5.5, 17.25 };

    std::printf("SceneBasis sample: one mesh shared by two instance nodes\n");
    std::printf("Basis B = [[1.25,0.35,0,2],[0,0.8,0,-1.5],[0,0,1.1,0.25],[0,0,0,1]]\n");
    for (double time : sampleTimes) {
        PrintInstance("before", time, scene, "InstanceA", 0);
        PrintInstance("before", time, scene, "InstanceB", 0);
    }

    Assimp::ChangeSceneBasis(scene, basis);

    for (double time : sampleTimes) {
        PrintInstance("after ", time, scene, "InstanceA", 0);
        PrintInstance("after ", time, scene, "InstanceB", 0);
    }

    // aiScene's destructor releases the node tree (including the auxiliary
    // nodes), the meshes and the animations.
    return 0;
}
