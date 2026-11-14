/*
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

/** @file Implementation of the morph target frame completion helpers.
 *
 *  The algorithms mirror GenVertexNormalsProcess and CalcTangentsProcess,
 *  but every target gets its own position-based spatial neighborhood and
 *  owns the arrays that are written. Base mesh streams and sibling targets
 *  are never modified or released.
 */

#include "MorphFrameCompletion.h"
#include "ProcessHelper.h"

#include <assimp/qnan.h>

#include <cmath>
#include <vector>

namespace Assimp {

namespace {

// ---------------------------------------------------------------------------
// Position epsilon for an arbitrary position array, matching
// ComputePositionEpsilon() but without requiring an aiMesh.
ai_real ComputeEpsilonForPositions(const aiVector3D *positions, unsigned int count) {
    aiVector3D minVec, maxVec;
    ArrayBounds(positions, count, minVec, maxVec);
    return (maxVec - minVec).Length() * ai_real(1e-4);
}

// ---------------------------------------------------------------------------
// True when every component of the vector holds a finite value.
bool IsFiniteVector(const aiVector3D &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

// ---------------------------------------------------------------------------
void CompleteMorphTargetNormals(aiMesh *mesh, ai_real maxSmoothAngleRad, bool reverseWinding) {
    if (mesh == nullptr || mesh->mNumAnimMeshes == 0 || mesh->mVertices == nullptr) {
        return;
    }

    // Lines and points have no well-defined face normals.
    if (!(mesh->mPrimitiveTypes & (aiPrimitiveType_TRIANGLE | aiPrimitiveType_POLYGON))) {
        return;
    }

    const ai_real qnan = std::numeric_limits<ai_real>::quiet_NaN();

    for (unsigned int t = 0; t < mesh->mNumAnimMeshes; ++t) {
        aiAnimMesh *target = mesh->mAnimMeshes[t];
        if (target == nullptr) {
            continue;
        }
        // Existing artist-authored normals are always preserved.
        if (target->mNormals != nullptr) {
            continue;
        }

        const aiVector3D *positions = target->mVertices != nullptr ? target->mVertices : mesh->mVertices;
        const unsigned int vertexCount = target->mNumVertices;

        // Per-face normals, stored per vertex exactly like the base kernel.
        aiVector3D *faceNormals = new aiVector3D[vertexCount];
        for (unsigned int i = 0; i < vertexCount; ++i) {
            faceNormals[i] = aiVector3D(qnan);
        }

        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace &face = mesh->mFaces[f];
            if (face.mNumIndices < 3) {
                for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                    faceNormals[face.mIndices[i]] = aiVector3D(qnan);
                }
                continue;
            }

            const aiVector3D *pV1 = &positions[face.mIndices[0]];
            const aiVector3D *pV2 = &positions[face.mIndices[1]];
            const aiVector3D *pV3 = &positions[face.mIndices[face.mNumIndices - 1]];
            if (reverseWinding) {
                std::swap(pV2, pV3);
            }

            if (!IsFiniteVector(*pV1) || !IsFiniteVector(*pV2) || !IsFiniteVector(*pV3)) {
                for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                    faceNormals[face.mIndices[i]] = aiVector3D(qnan);
                }
                continue;
            }

            const aiVector3D faceNormal = ((*pV2 - *pV1) ^ (*pV3 - *pV1)).NormalizeSafe();
            for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                faceNormals[face.mIndices[i]] = faceNormal;
            }
        }

        // The neighborhood is always rebuilt from this target's positions;
        // the base pose spatial sort must never be reused here.
        SpatialSort vertexFinder;
        vertexFinder.Fill(positions, vertexCount, sizeof(aiVector3D));
        const ai_real posEpsilon = ComputeEpsilonForPositions(positions, vertexCount);

        aiVector3D *smoothedNormals = new aiVector3D[vertexCount];
        std::vector<unsigned int> verticesFound;

        if (maxSmoothAngleRad >= AI_DEG_TO_RAD(175.f)) {
            std::vector<bool> hadVertex(vertexCount, false);
            for (unsigned int i = 0; i < vertexCount; ++i) {
                if (hadVertex[i]) {
                    continue;
                }

                vertexFinder.FindPositions(positions[i], posEpsilon, verticesFound);

                aiVector3D accumulated;
                for (size_t a = 0; a < verticesFound.size(); ++a) {
                    const aiVector3D &v = faceNormals[verticesFound[a]];
                    if (is_not_qnan(v.x)) {
                        accumulated += v;
                    }
                }
                accumulated.NormalizeSafe();

                for (size_t a = 0; a < verticesFound.size(); ++a) {
                    const unsigned int vidx = verticesFound[a];
                    smoothedNormals[vidx] = accumulated;
                    hadVertex[vidx] = true;
                }
            }
        } else {
            const ai_real limit = std::cos(maxSmoothAngleRad);
            for (unsigned int i = 0; i < vertexCount; ++i) {
                vertexFinder.FindPositions(positions[i], posEpsilon, verticesFound);

                const aiVector3D ownNormal = faceNormals[i];
                aiVector3D accumulated;
                for (size_t a = 0; a < verticesFound.size(); ++a) {
                    const aiVector3D &v = faceNormals[verticesFound[a]];
                    if (is_not_qnan(v.x) && (verticesFound[a] == i || (v * ownNormal >= limit))) {
                        accumulated += v;
                    }
                }
                smoothedNormals[i] = accumulated.NormalizeSafe();
            }
        }

        delete[] faceNormals;
        target->mNormals = smoothedNormals;
    }
}

// ---------------------------------------------------------------------------
void CompleteMorphTargetTangents(aiMesh *mesh, ai_real maxSmoothAngleRad, unsigned int sourceUV) {
    if (mesh == nullptr || mesh->mNumAnimMeshes == 0 || mesh->mVertices == nullptr) {
        return;
    }

    if (!(mesh->mPrimitiveTypes & (aiPrimitiveType_TRIANGLE | aiPrimitiveType_POLYGON))) {
        return;
    }
    if (sourceUV >= AI_MAX_NUMBER_OF_TEXTURECOORDS || mesh->mTextureCoords[sourceUV] == nullptr) {
        return;
    }

    const aiVector3D *meshTex = mesh->mTextureCoords[sourceUV];
    const ai_real angleEpsilon = 0.9999f;
    const ai_real qnan = get_qnan();

    for (unsigned int t = 0; t < mesh->mNumAnimMeshes; ++t) {
        aiAnimMesh *target = mesh->mAnimMeshes[t];
        if (target == nullptr) {
            continue;
        }
        // Existing tangent/bitangent pairs are always preserved. The streams
        // are specified to exist either both or neither, so skip as soon as
        // either half is already present rather than overwriting the other.
        if (target->mTangents != nullptr || target->mBitangents != nullptr) {
            continue;
        }
        // Normals are a hard requirement for tangent space computation.
        const aiVector3D *meshNorm = target->mNormals != nullptr ? target->mNormals : mesh->mNormals;
        if (meshNorm == nullptr) {
            continue;
        }

        const aiVector3D *meshPos = target->mVertices != nullptr ? target->mVertices : mesh->mVertices;
        const unsigned int vertexCount = target->mNumVertices;

        std::vector<bool> vertexDone(vertexCount, false);
        aiVector3D *meshTang = new aiVector3D[vertexCount];
        aiVector3D *meshBitang = new aiVector3D[vertexCount];

        // Face pass: project the per-face tangent frame onto each vertex normal.
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace &face = mesh->mFaces[f];
            if (face.mNumIndices < 3) {
                for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                    const unsigned int idx = face.mIndices[i];
                    vertexDone[idx] = true;
                    meshTang[idx] = aiVector3D(qnan);
                    meshBitang[idx] = aiVector3D(qnan);
                }
                continue;
            }

            const unsigned int p0 = face.mIndices[0];
            const unsigned int p1 = face.mIndices[1];
            const unsigned int p2 = face.mIndices[2];

            if (!IsFiniteVector(meshPos[p0]) || !IsFiniteVector(meshPos[p1]) || !IsFiniteVector(meshPos[p2])) {
                for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                    const unsigned int idx = face.mIndices[i];
                    vertexDone[idx] = true;
                    meshTang[idx] = aiVector3D(qnan);
                    meshBitang[idx] = aiVector3D(qnan);
                }
                continue;
            }

            aiVector3D v = meshPos[p1] - meshPos[p0];
            aiVector3D w = meshPos[p2] - meshPos[p0];

            ai_real sx = meshTex[p1].x - meshTex[p0].x;
            ai_real sy = meshTex[p1].y - meshTex[p0].y;
            ai_real tx = meshTex[p2].x - meshTex[p0].x;
            ai_real ty = meshTex[p2].y - meshTex[p0].y;
            ai_real dirCorrection = (tx * sy - ty * sx) < 0.0f ? -1.0f : 1.0f;
            // Degenerate UV triangle: fall back to the default UV direction.
            if (sx * ty == sy * tx) {
                sx = 0.0;
                sy = 1.0;
                tx = 1.0;
                ty = 0.0;
            }

            aiVector3D tangent, bitangent;
            tangent.x = (w.x * sy - v.x * ty) * dirCorrection;
            tangent.y = (w.y * sy - v.y * ty) * dirCorrection;
            tangent.z = (w.z * sy - v.z * ty) * dirCorrection;
            bitangent.x = (w.x * sx - v.x * tx) * dirCorrection;
            bitangent.y = (w.y * sx - v.y * tx) * dirCorrection;
            bitangent.z = (w.z * sx - v.z * tx) * dirCorrection;

            for (unsigned int b = 0; b < face.mNumIndices; ++b) {
                const unsigned int p = face.mIndices[b];

                aiVector3D localTangent = tangent - meshNorm[p] * (tangent * meshNorm[p]);
                aiVector3D localBitangent = bitangent - meshNorm[p] * (bitangent * meshNorm[p]);
                localTangent.NormalizeSafe();
                localBitangent.NormalizeSafe();

                bool invalidTangent = is_special_float(localTangent.x) || is_special_float(localTangent.y) ||
                        is_special_float(localTangent.z) ||
                        (-0.5f < localTangent.x && localTangent.x < 0.5f && -0.5f < localTangent.y &&
                                localTangent.y < 0.5f && -0.5f < localTangent.z && localTangent.z < 0.5f);
                bool invalidBitangent = is_special_float(localBitangent.x) || is_special_float(localBitangent.y) ||
                        is_special_float(localBitangent.z) ||
                        (-0.5f < localBitangent.x && localBitangent.x < 0.5f && -0.5f < localBitangent.y &&
                                localBitangent.y < 0.5f && -0.5f < localBitangent.z && localBitangent.z < 0.5f);
                if (invalidTangent != invalidBitangent) {
                    if (invalidTangent) {
                        localTangent = meshNorm[p] ^ localBitangent;
                        localTangent.NormalizeSafe();
                    } else {
                        localBitangent = localTangent ^ meshNorm[p];
                        localBitangent.NormalizeSafe();
                    }
                }

                meshTang[p] = localTangent;
                meshBitang[p] = localBitangent;
            }
        }

        // Smoothing pass over a spatial sort built from this target's positions.
        SpatialSort vertexFinder;
        vertexFinder.Fill(meshPos, vertexCount, sizeof(aiVector3D));
        const ai_real posEpsilon = ComputeEpsilonForPositions(meshPos, vertexCount);
        const ai_real limit = std::cos(maxSmoothAngleRad);

        std::vector<unsigned int> verticesFound;
        std::vector<unsigned int> closeVertices;
        for (unsigned int a = 0; a < vertexCount; ++a) {
            if (vertexDone[a]) {
                continue;
            }

            const aiVector3D &origPos = meshPos[a];
            const aiVector3D &origNorm = meshNorm[a];
            const aiVector3D &origTang = meshTang[a];
            const aiVector3D &origBitang = meshBitang[a];
            closeVertices.clear();

            vertexFinder.FindPositions(origPos, posEpsilon, verticesFound);
            closeVertices.reserve(verticesFound.size() + 5);
            closeVertices.push_back(a);

            for (size_t b = 0; b < verticesFound.size(); ++b) {
                const unsigned int idx = verticesFound[b];
                if (vertexDone[idx]) {
                    continue;
                }
                if (meshNorm[idx] * origNorm < angleEpsilon) {
                    continue;
                }
                if (meshTang[idx] * origTang < limit) {
                    continue;
                }
                if (meshBitang[idx] * origBitang < limit) {
                    continue;
                }
                closeVertices.push_back(idx);
                vertexDone[idx] = true;
            }

            aiVector3D smoothTangent(0, 0, 0), smoothBitangent(0, 0, 0);
            for (size_t b = 0; b < closeVertices.size(); ++b) {
                smoothTangent += meshTang[closeVertices[b]];
                smoothBitangent += meshBitang[closeVertices[b]];
            }
            smoothTangent.Normalize();
            smoothBitangent.Normalize();

            for (size_t b = 0; b < closeVertices.size(); ++b) {
                meshTang[closeVertices[b]] = smoothTangent;
                meshBitang[closeVertices[b]] = smoothBitangent;
            }
        }

        target->mTangents = meshTang;
        target->mBitangents = meshBitang;
    }
}

} // end of namespace Assimp
