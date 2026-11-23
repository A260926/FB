/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the following
conditions are met:

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
---------------------------------------------------------------------------
*/

/** @file Implementation of the post processing step that completes missing
 *  normal and tangent frames of morph targets (aiAnimMesh).
 */

// internal headers
#include "MorphFrameCompletionProcess.h"
#include "ProcessHelper.h"
#include <assimp/Importer.hpp>
#include <assimp/SpatialSort.h>
#include <assimp/config.h>
#include <assimp/qnan.h>

using namespace Assimp;

// ------------------------------------------------------------------------------------------------
// Constructor to be privately used by Importer
MorphFrameCompletionProcess::MorphFrameCompletionProcess() :
        configNormalsMaxAngle(AI_DEG_TO_RAD(175.f)),
        configTangentsMaxAngle(float(AI_DEG_TO_RAD(45.f))),
        configEnabled(false) {
    // nothing to do here
}

// ------------------------------------------------------------------------------------------------
// Returns whether the processing step is present in the given flag field.
bool MorphFrameCompletionProcess::IsActive(unsigned int pFlags) const {
    genNormalsRequested_ = (pFlags & aiProcess_GenSmoothNormals) != 0;
    calcTangentsRequested_ = (pFlags & aiProcess_CalcTangentSpace) != 0;
    flippedWindingOrder_ = (pFlags & aiProcess_FlipWindingOrder) != 0;
    leftHanded_ = (pFlags & aiProcess_MakeLeftHanded) != 0;
    return genNormalsRequested_ || calcTangentsRequested_;
}

// ------------------------------------------------------------------------------------------------
// Updates the step configuration from the importer properties.
void MorphFrameCompletionProcess::SetupProperties(const Importer *pImp) {
    ai_assert(nullptr != pImp);

    // the step is opt-in via AI_CONFIG_PP_MORPH_FRAME_COMPLETION
    configEnabled = pImp->GetPropertyBool(AI_CONFIG_PP_MORPH_FRAME_COMPLETION, false);

    // the smoothing angles follow the same rules as the base kernels
    configNormalsMaxAngle = pImp->GetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, (ai_real)175.0);
    configNormalsMaxAngle = AI_DEG_TO_RAD(std::max(std::min(configNormalsMaxAngle, (ai_real)175.0), (ai_real)0.0));

    configTangentsMaxAngle = pImp->GetPropertyFloat(AI_CONFIG_PP_CT_MAX_SMOOTHING_ANGLE, 45.f);
    configTangentsMaxAngle = std::max(std::min(configTangentsMaxAngle, 45.0f), 0.0f);
    configTangentsMaxAngle = AI_DEG_TO_RAD(configTangentsMaxAngle);
}

// ------------------------------------------------------------------------------------------------
// Executes the post processing step on the given imported data.
void MorphFrameCompletionProcess::Execute(aiScene *pScene) {
    ai_assert(nullptr != pScene);

    if (!configEnabled) {
        return;
    }

    ASSIMP_LOG_DEBUG("MorphFrameCompletionProcess begin");

    bool bHas = false;
    for (unsigned int a = 0; a < pScene->mNumMeshes; ++a) {
        aiMesh *pMesh = pScene->mMeshes[a];
        if (nullptr == pMesh || 0 == pMesh->mNumAnimMeshes) {
            continue;
        }
        for (unsigned int am = 0; am < pMesh->mNumAnimMeshes; ++am) {
            aiAnimMesh *pAnimMesh = pMesh->mAnimMeshes[am];
            if (nullptr == pAnimMesh) {
                continue;
            }
            // The normal pass runs first so that a completed normal frame
            // can feed the tangent pass of the same target. Every target is
            // completed independently; existing frames are never rebuilt.
            if (genNormalsRequested_ && GenAnimMeshNormals(pMesh, pAnimMesh)) {
                bHas = true;
            }
            if (calcTangentsRequested_ && ProcessAnimMeshTangents(pMesh, pAnimMesh)) {
                bHas = true;
            }
        }
    }

    if (bHas) {
        ASSIMP_LOG_INFO("MorphFrameCompletionProcess finished. "
                        "Morph target frames have been completed");
    } else {
        ASSIMP_LOG_DEBUG("MorphFrameCompletionProcess finished. "
                         "Morph target frames are already complete");
    }
}

// ------------------------------------------------------------------------------------------------
// Generates smooth vertex normals for a single morph target.
bool MorphFrameCompletionProcess::GenAnimMeshNormals(aiMesh *pMesh, aiAnimMesh *pAnimMesh) {
    // Frames that are already there (artist-authored or generated) are kept.
    if (nullptr != pAnimMesh->mNormals) {
        return false;
    }

    // Same restriction as the base kernel: normal vectors are undefined
    // for line and point meshes.
    if (!(pMesh->mPrimitiveTypes & (aiPrimitiveType_TRIANGLE | aiPrimitiveType_POLYGON))) {
        return false;
    }

    // Targets without own positions are evaluated against the base positions.
    const aiVector3D *positions = pAnimMesh->HasPositions() ? pAnimMesh->mVertices : pMesh->mVertices;
    if (nullptr == positions) {
        return false;
    }
    const unsigned int numVertices = pMesh->mNumVertices;

    // Compute per-face normals but store them per-vertex, exactly like
    // GenVertexNormalsProcess does for the base mesh.
    const float qnan = std::numeric_limits<ai_real>::quiet_NaN();
    std::vector<aiVector3D> faceNormals(numVertices);
    for (unsigned int a = 0; a < pMesh->mNumFaces; a++) {
        const aiFace &face = pMesh->mFaces[a];
        if (face.mNumIndices < 3) {
            // either a point or a line -> no normal vector
            for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                faceNormals[face.mIndices[i]] = aiVector3D(qnan);
            }

            continue;
        }

        const aiVector3D *pV1 = &positions[face.mIndices[0]];
        const aiVector3D *pV2 = &positions[face.mIndices[1]];
        const aiVector3D *pV3 = &positions[face.mIndices[face.mNumIndices - 1]];
        // Boolean XOR - if either but not both of these flags is set, then the winding order has
        // changed and the cross product to calculate the normal needs to be reversed
        if (flippedWindingOrder_ != leftHanded_) {
            std::swap(pV2, pV3);
        }
        const aiVector3D vNor = ((*pV2 - *pV1) ^ (*pV3 - *pV1)).NormalizeSafe();

        for (unsigned int i = 0; i < face.mNumIndices; ++i) {
            faceNormals[face.mIndices[i]] = vNor;
        }
    }

    // Set up a SpatialSort on the target's own positions. The base pose
    // grouping is never reused: two targets of the same mesh may cluster
    // their vertices completely differently.
    SpatialSort vertexFinder;
    vertexFinder.Fill(positions, numVertices, sizeof(aiVector3D));
    aiVector3D minVec, maxVec;
    ArrayBounds(positions, numVertices, minVec, maxVec);
    const ai_real posEpsilon = (maxVec - minVec).Length() * ai_real(1e-4);

    std::vector<unsigned int> verticesFound;
    aiVector3D *pcNew = new aiVector3D[numVertices];

    if (configNormalsMaxAngle >= AI_DEG_TO_RAD(175.f)) {
        // There is no angle limit. Thus all vertices with positions close
        // to each other will receive the same vertex normal. This allows us
        // to optimize the whole algorithm a little bit ...
        std::vector<bool> abHad(numVertices, false);
        for (unsigned int i = 0; i < numVertices; ++i) {
            if (abHad[i]) {
                continue;
            }

            // Get all vertices that share this one ...
            vertexFinder.FindPositions(positions[i], posEpsilon, verticesFound);

            aiVector3D pcNor;
            for (unsigned int a = 0; a < verticesFound.size(); ++a) {
                const aiVector3D &v = faceNormals[verticesFound[a]];
                if (is_not_qnan(v.x)) pcNor += v;
            }
            pcNor.NormalizeSafe();

            // Write the smoothed normal back to all affected normals
            for (unsigned int a = 0; a < verticesFound.size(); ++a) {
                unsigned int vidx = verticesFound[a];
                pcNew[vidx] = pcNor;
                abHad[vidx] = true;
            }
        }
    }
    // Slower code path if a smooth angle is set. There are many ways to achieve
    // the effect, this one is the most straightforward one.
    else {
        const ai_real fLimit = std::cos(configNormalsMaxAngle);
        for (unsigned int i = 0; i < numVertices; ++i) {
            // Get all vertices that share this one ...
            vertexFinder.FindPositions(positions[i], posEpsilon, verticesFound);

            aiVector3D vr = faceNormals[i];

            aiVector3D pcNor;
            for (unsigned int a = 0; a < verticesFound.size(); ++a) {
                aiVector3D v = faceNormals[verticesFound[a]];

                // Check whether the angle between the two normals is not too large.
                // Skip the angle check on our own normal to avoid false negatives
                // (v*v is not guaranteed to be 1.0 for all unit vectors v)
                if (is_not_qnan(v.x) && (verticesFound[a] == i || (v * vr >= fLimit)))
                    pcNor += v;
            }
            pcNew[i] = pcNor.NormalizeSafe();
        }
    }

    // The completed frame is owned by the target itself.
    pAnimMesh->mNormals = pcNew;

    return true;
}

// ------------------------------------------------------------------------------------------------
// Generates tangents and bitangents for a single morph target.
bool MorphFrameCompletionProcess::ProcessAnimMeshTangents(aiMesh *pMesh, aiAnimMesh *pAnimMesh) {
    // Tangents and bitangents always go together. Frames that are already
    // there (artist-authored or generated) are kept.
    if (nullptr != pAnimMesh->mTangents || nullptr != pAnimMesh->mBitangents) {
        return false;
    }

    // Same restriction as the base kernel: tangents are undefined for
    // line and point meshes.
    if (!(pMesh->mPrimitiveTypes & (aiPrimitiveType_TRIANGLE | aiPrimitiveType_POLYGON))) {
        return false;
    }

    // Gather the effective input frames: the target's own data wins,
    // missing streams are inherited from the base mesh. The texture
    // coordinates are always taken from the base mesh's first UV channel.
    const aiVector3D *meshPos = pAnimMesh->HasPositions() ? pAnimMesh->mVertices : pMesh->mVertices;
    const aiVector3D *meshNorm = pAnimMesh->HasNormals() ? pAnimMesh->mNormals : pMesh->mNormals;
    const aiVector3D *meshTex = pMesh->mTextureCoords[0];
    if (nullptr == meshPos || nullptr == meshNorm) {
        ASSIMP_LOG_ERROR("Failed to compute tangents for morph target; need positions and normals");
        return false;
    }
    if (nullptr == meshTex) {
        ASSIMP_LOG_ERROR("Failed to compute tangents for morph target; need UV data in channel 0");
        return false;
    }

    const unsigned int numVertices = pMesh->mNumVertices;
    const float angleEpsilon = 0.9999f;

    std::vector<bool> vertexDone(numVertices, false);
    const float qnan = get_qnan();

    // create space for the tangents and bitangents, owned by the target
    pAnimMesh->mTangents = new aiVector3D[numVertices];
    pAnimMesh->mBitangents = new aiVector3D[numVertices];

    aiVector3D *meshTang = pAnimMesh->mTangents;
    aiVector3D *meshBitang = pAnimMesh->mBitangents;

    // calculate the tangent and bitangent for every face
    for (unsigned int a = 0; a < pMesh->mNumFaces; a++) {
        const aiFace &face = pMesh->mFaces[a];
        if (face.mNumIndices < 3) {
            // There are less than three indices, thus the tangent vector
            // is not defined. We are finished with these vertices now,
            // their tangent vectors are set to qnan.
            for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                unsigned int idx = face.mIndices[i];
                vertexDone[idx] = true;
                meshTang[idx] = aiVector3D(qnan);
                meshBitang[idx] = aiVector3D(qnan);
            }

            continue;
        }

        // triangle or polygon... we always use only the first three indices. A polygon
        // is supposed to be planar anyways....
        const unsigned int p0 = face.mIndices[0], p1 = face.mIndices[1], p2 = face.mIndices[2];

        // position differences p1->p2 and p1->p3
        aiVector3D v = meshPos[p1] - meshPos[p0], w = meshPos[p2] - meshPos[p0];

        // texture offset p1->p2 and p1->p3
        float sx = meshTex[p1].x - meshTex[p0].x, sy = meshTex[p1].y - meshTex[p0].y;
        float tx = meshTex[p2].x - meshTex[p0].x, ty = meshTex[p2].y - meshTex[p0].y;
        float dirCorrection = (tx * sy - ty * sx) < 0.0f ? -1.0f : 1.0f;
        // when t1, t2, t3 in same position in UV space, just use default UV direction.
        if (sx * ty == sy * tx) {
            sx = 0.0;
            sy = 1.0;
            tx = 1.0;
            ty = 0.0;
        }

        // tangent points in the direction where to positive X axis of the texture coord's would point in model space
        // bitangent's points along the positive Y axis of the texture coord's, respectively
        aiVector3D tangent, bitangent;
        tangent.x = (w.x * sy - v.x * ty) * dirCorrection;
        tangent.y = (w.y * sy - v.y * ty) * dirCorrection;
        tangent.z = (w.z * sy - v.z * ty) * dirCorrection;
        bitangent.x = (w.x * sx - v.x * tx) * dirCorrection;
        bitangent.y = (w.y * sx - v.y * tx) * dirCorrection;
        bitangent.z = (w.z * sx - v.z * tx) * dirCorrection;

        // store for every vertex of that face
        for (unsigned int b = 0; b < face.mNumIndices; ++b) {
            unsigned int p = face.mIndices[b];

            // project tangent and bitangent into the plane formed by the vertex' normal
            aiVector3D localTangent = tangent - meshNorm[p] * (tangent * meshNorm[p]);
            aiVector3D localBitangent = bitangent - meshNorm[p] * (bitangent * meshNorm[p]);
            localTangent.NormalizeSafe();
            localBitangent.NormalizeSafe();

            // reconstruct tangent/bitangent according to normal and bitangent/tangent when it's infinite or NaN.
            bool invalid_tangent = is_special_float(localTangent.x) || is_special_float(localTangent.y) || is_special_float(localTangent.z)
                || (-0.5f < localTangent.x && localTangent.x < 0.5f && -0.5f < localTangent.y && localTangent.y < 0.5f && -0.5f < localTangent.z && localTangent.z < 0.5f);
            bool invalid_bitangent = is_special_float(localBitangent.x) || is_special_float(localBitangent.y) || is_special_float(localBitangent.z)
                || (-0.5f < localBitangent.x && localBitangent.x < 0.5f && -0.5f < localBitangent.y && localBitangent.y < 0.5f && -0.5f < localBitangent.z && localBitangent.z < 0.5f);
            if (invalid_tangent != invalid_bitangent) {
                if (invalid_tangent) {
                    localTangent = meshNorm[p] ^ localBitangent;
                    localTangent.NormalizeSafe();
                } else {
                    localBitangent = localTangent ^ meshNorm[p];
                    localBitangent.NormalizeSafe();
                }
            }

            // and write it into the morph target.
            meshTang[p] = localTangent;
            meshBitang[p] = localBitangent;
        }
    }

    // create a helper to quickly find locally close vertices among the vertex
    // array. The neighborhood is built from the target's own positions, the
    // base pose grouping is never reused.
    SpatialSort vertexFinder;
    vertexFinder.Fill(meshPos, numVertices, sizeof(aiVector3D));
    aiVector3D minVec, maxVec;
    ArrayBounds(meshPos, numVertices, minVec, maxVec);
    const float posEpsilon = (maxVec - minVec).Length() * 1e-4f;
    std::vector<unsigned int> verticesFound;

    const float fLimit = std::cos(configTangentsMaxAngle);
    std::vector<unsigned int> closeVertices;

    // in the second pass we now smooth out all tangents and bitangents at the same local position
    // if they are not too far off.
    for (unsigned int a = 0; a < numVertices; a++) {
        if (vertexDone[a])
            continue;

        const aiVector3D &origPos = meshPos[a];
        const aiVector3D &origNorm = meshNorm[a];
        const aiVector3D &origTang = meshTang[a];
        const aiVector3D &origBitang = meshBitang[a];
        closeVertices.resize(0);

        // find all vertices close to that position
        vertexFinder.FindPositions(origPos, posEpsilon, verticesFound);

        closeVertices.reserve(verticesFound.size() + 5);
        closeVertices.push_back(a);

        // look among them for other vertices sharing the same normal and a close-enough tangent/bitangent
        for (unsigned int b = 0; b < verticesFound.size(); b++) {
            unsigned int idx = verticesFound[b];
            if (vertexDone[idx])
                continue;
            if (meshNorm[idx] * origNorm < angleEpsilon)
                continue;
            if (meshTang[idx] * origTang < fLimit)
                continue;
            if (meshBitang[idx] * origBitang < fLimit)
                continue;

            // it's similar enough -> add it to the smoothing group
            closeVertices.push_back(idx);
            vertexDone[idx] = true;
        }

        // smooth the tangents and bitangents of all vertices that were found to be close enough
        aiVector3D smoothTangent(0, 0, 0), smoothBitangent(0, 0, 0);
        for (unsigned int b = 0; b < closeVertices.size(); ++b) {
            smoothTangent += meshTang[closeVertices[b]];
            smoothBitangent += meshBitang[closeVertices[b]];
        }
        smoothTangent.Normalize();
        smoothBitangent.Normalize();

        // and write it back into all affected tangents
        for (unsigned int b = 0; b < closeVertices.size(); ++b) {
            meshTang[closeVertices[b]] = smoothTangent;
            meshBitang[closeVertices[b]] = smoothBitangent;
        }
    }

    return true;
}
