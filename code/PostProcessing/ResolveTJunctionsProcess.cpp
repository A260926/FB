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

/** @file  ResolveTJunctionsProcess.cpp
 *  @brief Mesh-local T-junction resolution: points falling into the
 *    interior of indexed edges are inserted as real boundary vertices
 *    before the affected polygons are triangulated.
 */
#ifndef ASSIMP_BUILD_NO_TRIANGULATE_PROCESS

#include "PostProcessing/ResolveTJunctionsProcess.h"

#include <assimp/color4.h>
#include <assimp/mesh.h>
#include <assimp/vector3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

using namespace Assimp;

namespace {

using EdgeKey = std::pair<unsigned int, unsigned int>;

EdgeKey MakeEdgeKey(unsigned int a, unsigned int b) {
    return (a < b) ? EdgeKey(a, b) : EdgeKey(b, a);
}

// Grow a per-vertex stream by `extra` default-constructed entries,
// preserving the existing values and taking ownership of the new array.
template <typename T>
T *GrowStream(T *oldArray, unsigned int oldCount, unsigned int extra) {
    if (nullptr == oldArray || extra == 0) {
        return oldArray;
    }
    T *grown = new T[oldCount + extra]();
    std::copy(oldArray, oldArray + oldCount, grown);
    delete[] oldArray;
    return grown;
}

ai_real Lerp(ai_real a, ai_real b, ai_real t) {
    return a + (b - a) * t;
}

aiVector3D LerpVec(const aiVector3D &a, const aiVector3D &b, ai_real t) {
    return aiVector3D(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t));
}

aiColor4D LerpColor(const aiColor4D &a, const aiColor4D &b, ai_real t) {
    return aiColor4D(Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t),
            Lerp(a.a, b.a, t));
}

// Sparse bone weight set of one vertex: bone index -> weight.
using WeightMap = std::map<unsigned int, ai_real>;

WeightMap BuildWeightMap(const aiMesh *mesh, unsigned int vertex) {
    WeightMap result;
    for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
        const aiBone *bone = mesh->mBones[b];
        for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
            const aiVertexWeight &vw = bone->mWeights[w];
            if (vw.mVertexId == vertex) {
                result.emplace(b, vw.mWeight);
                break;
            }
        }
    }
    return result;
}

WeightMap LerpWeightMap(const WeightMap &a, const WeightMap &b, ai_real t) {
    WeightMap merged;
    for (const auto &entry : a) {
        merged.emplace(entry.first, Lerp(entry.second, ai_real(0.0), t));
    }
    for (const auto &entry : b) {
        const auto found = a.find(entry.first);
        const ai_real wa = (found == a.end()) ? ai_real(0.0) : found->second;
        merged[entry.first] = Lerp(wa, entry.second, t);
    }
    return merged;
}

// 2D cross product (b-a) x (c-a) on the chosen projection axes.
ai_real Cross2D(const aiVector3D &a, const aiVector3D &b, const aiVector3D &c,
        unsigned int axisA, unsigned int axisB) {
    const ai_real bax = b[axisA] - a[axisA];
    const ai_real bay = b[axisB] - a[axisB];
    const ai_real cax = c[axisA] - a[axisA];
    const ai_real cay = c[axisB] - a[axisB];
    return bax * cay - bay * cax;
}

// Ear clipping for a simple planar polygon given as an ordered ring.
// Every output triangle keeps the winding of the input ring.
std::vector<std::array<unsigned int, 3>> EarClipRing(
        const std::vector<aiVector3D> &points, unsigned int axisA, unsigned int axisB) {
    std::vector<std::array<unsigned int, 3>> triangles;
    const size_t count = points.size();
    if (count < 3) {
        return triangles;
    }

    ai_real area2x = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const aiVector3D &a = points[i];
        const aiVector3D &b = points[(i + 1) % count];
        area2x += a[axisA] * b[axisB] - b[axisA] * a[axisB];
    }
    const ai_real orientation = (area2x > 0.0) ? ai_real(1.0) : ai_real(-1.0);

    std::vector<unsigned int> ring(count);
    for (size_t i = 0; i < count; ++i) {
        ring[i] = static_cast<unsigned int>(i);
    }

    auto isConvex = [&](unsigned int prev, unsigned int cur, unsigned int next) {
        return Cross2D(points[prev], points[cur], points[next], axisA, axisB) * orientation > 0.0;
    };
    auto blocksEar = [&](unsigned int ta, unsigned int tb, unsigned int tc,
                             unsigned int p, unsigned int pPrev, unsigned int pNext) {
        // Inclusive triangle test in the ring's winding orientation.
        const ai_real d1 = Cross2D(points[ta], points[tb], points[p], axisA, axisB) * orientation;
        const ai_real d2 = Cross2D(points[tb], points[tc], points[p], axisA, axisB) * orientation;
        const ai_real d3 = Cross2D(points[tc], points[ta], points[p], axisA, axisB) * orientation;
        if (d1 < 0.0 || d2 < 0.0 || d3 < 0.0) {
            return false; // strictly outside the candidate ear
        }
        if (d1 > 0.0 && d2 > 0.0 && d3 > 0.0) {
            return true; // strictly inside: always blocks
        }
        // On the triangle boundary: collinear boundary split points must
        // not block, but a reflex vertex touching the diagonal must.
        return Cross2D(points[pPrev], points[p], points[pNext], axisA, axisB) * orientation < 0.0;
    };

    size_t guard = 0;
    while (ring.size() > 3 && guard++ < count * count + 16) {
        bool clipped = false;
        const size_t n = ring.size();
        for (size_t k = 0; k < n; ++k) {
            const unsigned int prev = ring[(k + n - 1) % n];
            const unsigned int cur = ring[k];
            const unsigned int next = ring[(k + 1) % n];
            if (!isConvex(prev, cur, next)) {
                continue;
            }
            bool hasBlockingPoint = false;
            for (size_t q = 0; q < n; ++q) {
                const unsigned int p = ring[q];
                if (p == prev || p == cur || p == next) {
                    continue;
                }
                const unsigned int pPrev = ring[(q + n - 1) % n];
                const unsigned int pNext = ring[(q + 1) % n];
                if (blocksEar(prev, cur, next, p, pPrev, pNext)) {
                    hasBlockingPoint = true;
                    break;
                }
            }
            if (hasBlockingPoint) {
                continue;
            }
            triangles.push_back({ prev, cur, next });
            ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(k));
            clipped = true;
            break;
        }
        if (!clipped) {
            break; // unreachable for valid simple input; avoids an endless loop
        }
    }
    if (ring.size() == 3) {
        triangles.push_back({ ring[0], ring[1], ring[2] });
    }
    return triangles;
}

} // namespace

// ------------------------------------------------------------------------------------------------
bool ResolveTJunctionsProcess::IsValidEpsilon(ai_real eps) {
    return std::isfinite(eps) && eps > ai_real(0.0);
}

// ------------------------------------------------------------------------------------------------
std::map<EdgeKey, ResolveTJunctionsProcess::EdgeSplit>
ResolveTJunctionsProcess::CollectEdgeSplits(const aiMesh *mesh, ai_real eps) {
    std::map<EdgeKey, EdgeSplit> result;
    if (nullptr == mesh || nullptr == mesh->mVertices) {
        return result;
    }

    // Faces with at least three indices define both the search edges and
    // the candidate vertex pool.
    std::vector<unsigned int> candidates;
    candidates.reserve(mesh->mNumVertices);
    std::vector<bool> referenced(mesh->mNumVertices, false);
    std::vector<EdgeKey> edges;
    edges.reserve(mesh->mNumFaces * 2);

    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices < 3) {
            continue;
        }
        for (unsigned int k = 0; k < face.mNumIndices; ++k) {
            const unsigned int v = face.mIndices[k];
            if (!referenced[v]) {
                referenced[v] = true;
                candidates.push_back(v);
            }
            edges.push_back(MakeEdgeKey(v, face.mIndices[(k + 1) % face.mNumIndices]));
        }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    const aiVector3D *vertices = mesh->mVertices;
    for (const EdgeKey &key : edges) {
        const aiVector3D &a = vertices[key.first];
        const aiVector3D &b = vertices[key.second];
        const aiVector3D ab = b - a;
        const ai_real lengthSquared = ab * ab;
        if (!(lengthSquared > ai_real(0.0))) {
            continue;
        }
        EdgeSplit split;
        split.endpointA = key.first;
        split.endpointB = key.second;
        for (unsigned int candidate : candidates) {
            if (candidate == key.first || candidate == key.second) {
                continue;
            }
            const aiVector3D ap = vertices[candidate] - a;
            const ai_real t = (ap * ab) / lengthSquared;
            if (!(t > ai_real(0.0) && t < ai_real(1.0))) {
                continue;
            }
            const aiVector3D closest = a + ab * t;
            const ai_real distance = (vertices[candidate] - closest).Length();
            if (distance <= eps) {
                split.points.push_back(SplitPoint{ candidate, t });
            }
        }
        if (!split.points.empty()) {
            std::sort(split.points.begin(), split.points.end(),
                    [](const SplitPoint &x, const SplitPoint &y) { return x.parameter < y.parameter; });

            // Verbose meshes can reference the same physical point through
            // several vertex indices. Distinct real split points are
            // guaranteed to be at least 10*epsilon apart, so candidates
            // closer than 2*epsilon are duplicates of one logical point.
            std::vector<SplitPoint> unique;
            for (SplitPoint &candidate : split.points) {
                if (!unique.empty() &&
                        (vertices[candidate.vertex] - vertices[unique.back().vertex]).Length() <=
                                ai_real(2.0) * eps) {
                    unique.back().equivalents.push_back(candidate.vertex);
                } else {
                    candidate.equivalents.push_back(candidate.vertex);
                    unique.push_back(std::move(candidate));
                }
            }
            split.points = std::move(unique);
            result.emplace(key, std::move(split));
        }
    }
    return result;
}

// ------------------------------------------------------------------------------------------------
aiVector3D ResolveTJunctionsProcess::LerpNormalized(
        const aiVector3D &a, const aiVector3D &b, ai_real t) {
    aiVector3D v = LerpVec(a, b, t);
    if (v.SquareLength() == ai_real(0.0)) {
        return aiVector3D(0.0, 0.0, 0.0); // zero vectors stay zero
    }
    v.Normalize();
    return v;
}

// ------------------------------------------------------------------------------------------------
void ResolveTJunctionsProcess::InterpolateAnimMesh(aiAnimMesh *anim,
        unsigned int newVertex, unsigned int vertexA, unsigned int vertexB, ai_real t) {
    if (nullptr == anim) {
        return;
    }
    if (anim->mVertices) {
        anim->mVertices[newVertex] = LerpVec(anim->mVertices[vertexA], anim->mVertices[vertexB], t);
    }
    if (anim->mNormals) {
        anim->mNormals[newVertex] =
                LerpNormalized(anim->mNormals[vertexA], anim->mNormals[vertexB], t);
    }
    if (anim->mTangents) {
        anim->mTangents[newVertex] =
                LerpNormalized(anim->mTangents[vertexA], anim->mTangents[vertexB], t);
    }
    if (anim->mBitangents) {
        anim->mBitangents[newVertex] =
                LerpNormalized(anim->mBitangents[vertexA], anim->mBitangents[vertexB], t);
    }
    for (unsigned int slot = 0; slot < AI_MAX_NUMBER_OF_COLOR_SETS; ++slot) {
        if (anim->mColors[slot]) {
            anim->mColors[slot][newVertex] =
                    LerpColor(anim->mColors[slot][vertexA], anim->mColors[slot][vertexB], t);
        }
    }
    for (unsigned int slot = 0; slot < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++slot) {
        if (anim->mTextureCoords[slot]) {
            anim->mTextureCoords[slot][newVertex] = LerpVec(
                    anim->mTextureCoords[slot][vertexA], anim->mTextureCoords[slot][vertexB], t);
        }
    }
}

// ------------------------------------------------------------------------------------------------
void ResolveTJunctionsProcess::InterpolateBoneWeights(aiMesh *mesh,
        unsigned int newVertex, unsigned int vertexA, unsigned int vertexB, ai_real t) {
    if (0 == mesh->mNumBones || nullptr == mesh->mBones) {
        return;
    }
    const WeightMap weightsA = BuildWeightMap(mesh, vertexA);
    const WeightMap weightsB = BuildWeightMap(mesh, vertexB);
    const WeightMap interpolated = LerpWeightMap(weightsA, weightsB, t);
    for (const auto &entry : interpolated) {
        if (entry.second == ai_real(0.0)) {
            continue; // zero entries are omitted, weights are not renormalized
        }
        aiBone *bone = mesh->mBones[entry.first];
        aiVertexWeight *grown = new aiVertexWeight[bone->mNumWeights + 1];
        std::copy(bone->mWeights, bone->mWeights + bone->mNumWeights, grown);
        grown[bone->mNumWeights] = aiVertexWeight(newVertex, entry.second);
        delete[] bone->mWeights;
        bone->mWeights = grown;
        ++bone->mNumWeights;
    }
}

// ------------------------------------------------------------------------------------------------
unsigned int ResolveTJunctionsProcess::AppendInterpolatedVertex(
        aiMesh *mesh, const EdgeInfo &edge, ai_real t) {
    const unsigned int vertexA = edge.low;
    const unsigned int vertexB = edge.high;
    const unsigned int newIndex = mesh->mNumVertices;

    mesh->mVertices = GrowStream(mesh->mVertices, newIndex, 1);
    mesh->mVertices[newIndex] = LerpVec(mesh->mVertices[vertexA], mesh->mVertices[vertexB], t);

    if (mesh->mNormals) {
        mesh->mNormals = GrowStream(mesh->mNormals, newIndex, 1);
        mesh->mNormals[newIndex] =
                LerpNormalized(mesh->mNormals[vertexA], mesh->mNormals[vertexB], t);
    }
    if (mesh->mTangents) {
        mesh->mTangents = GrowStream(mesh->mTangents, newIndex, 1);
        mesh->mTangents[newIndex] =
                LerpNormalized(mesh->mTangents[vertexA], mesh->mTangents[vertexB], t);
    }
    if (mesh->mBitangents) {
        mesh->mBitangents = GrowStream(mesh->mBitangents, newIndex, 1);
        mesh->mBitangents[newIndex] =
                LerpNormalized(mesh->mBitangents[vertexA], mesh->mBitangents[vertexB], t);
    }
    for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_COLOR_SETS; ++channel) {
        if (mesh->mColors[channel]) {
            mesh->mColors[channel] = GrowStream(mesh->mColors[channel], newIndex, 1);
            mesh->mColors[channel][newIndex] =
                    LerpColor(mesh->mColors[channel][vertexA], mesh->mColors[channel][vertexB], t);
        }
    }
    for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++channel) {
        if (mesh->mTextureCoords[channel]) {
            mesh->mTextureCoords[channel] =
                    GrowStream(mesh->mTextureCoords[channel], newIndex, 1);
            mesh->mTextureCoords[channel][newIndex] = LerpVec(
                    mesh->mTextureCoords[channel][vertexA], mesh->mTextureCoords[channel][vertexB], t);
        }
    }

    // Every non-null anim stream is extended with the same parameter;
    // null streams remain null and keep independent ownership.
    for (unsigned int a = 0; a < mesh->mNumAnimMeshes; ++a) {
        aiAnimMesh *anim = mesh->mAnimMeshes[a];
        if (nullptr == anim) {
            continue;
        }
        if (anim->mVertices) {
            anim->mVertices = GrowStream(anim->mVertices, newIndex, 1);
        }
        if (anim->mNormals) {
            anim->mNormals = GrowStream(anim->mNormals, newIndex, 1);
        }
        if (anim->mTangents) {
            anim->mTangents = GrowStream(anim->mTangents, newIndex, 1);
        }
        if (anim->mBitangents) {
            anim->mBitangents = GrowStream(anim->mBitangents, newIndex, 1);
        }
        for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_COLOR_SETS; ++channel) {
            if (anim->mColors[channel]) {
                anim->mColors[channel] = GrowStream(anim->mColors[channel], newIndex, 1);
            }
        }
        for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++channel) {
            if (anim->mTextureCoords[channel]) {
                anim->mTextureCoords[channel] =
                        GrowStream(anim->mTextureCoords[channel], newIndex, 1);
            }
        }
        anim->mNumVertices = newIndex + 1;
        InterpolateAnimMesh(anim, newIndex, vertexA, vertexB, t);
    }

    InterpolateBoneWeights(mesh, newIndex, vertexA, vertexB, t);

    mesh->mNumVertices = newIndex + 1;
    return newIndex;
}

// ------------------------------------------------------------------------------------------------
bool ResolveTJunctionsProcess::ApplyToMesh(aiMesh *mesh,
        const std::map<EdgeKey, EdgeSplit> &splits) {
    if (nullptr == mesh || splits.empty()) {
        return false;
    }

    const unsigned int originalVertexCount = mesh->mNumVertices;

    // Per-edge node tables. A node slot initially remembers the candidate
    // vertex index so the reuse check can compare against it; once the
    // final vertex index is known it is overwritten.
    std::map<EdgeKey, EdgeInfo> edgeTable;
    for (const auto &entry : splits) {
        EdgeInfo info;
        info.low = entry.first.first;
        info.high = entry.first.second;
        info.splits = entry.second.points;
        info.nodesLowToHigh.push_back(info.low);
        for (const SplitPoint &sp : info.splits) {
            info.nodesLowToHigh.push_back(sp.vertex); // temporary: primary candidate id
        }
        info.nodesLowToHigh.push_back(info.high);
        edgeTable.emplace(entry.first, std::move(info));
    }

    // Sparse bone weight tables for every original vertex; new vertices
    // get their weights appended directly while the arrays are grown.
    std::vector<WeightMap> originalWeights(originalVertexCount);
    if (mesh->mNumBones) {
        for (unsigned int v = 0; v < originalVertexCount; ++v) {
            originalWeights[v] = BuildWeightMap(mesh, v);
        }
    }

    auto baseStreamMatches = [&](unsigned int candidate, unsigned int va, unsigned int vb,
                                     ai_real t) {
        if (mesh->mVertices[candidate] != LerpVec(mesh->mVertices[va], mesh->mVertices[vb], t)) {
            return false;
        }
        if (mesh->mNormals &&
                mesh->mNormals[candidate] !=
                        LerpNormalized(mesh->mNormals[va], mesh->mNormals[vb], t)) {
            return false;
        }
        if (mesh->mTangents &&
                mesh->mTangents[candidate] !=
                        LerpNormalized(mesh->mTangents[va], mesh->mTangents[vb], t)) {
            return false;
        }
        if (mesh->mBitangents &&
                mesh->mBitangents[candidate] !=
                        LerpNormalized(mesh->mBitangents[va], mesh->mBitangents[vb], t)) {
            return false;
        }
        for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_COLOR_SETS; ++channel) {
            if (mesh->mColors[channel] &&
                    mesh->mColors[channel][candidate] !=
                            LerpColor(mesh->mColors[channel][va], mesh->mColors[channel][vb], t)) {
                return false;
            }
        }
        for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++channel) {
            if (mesh->mTextureCoords[channel] &&
                    mesh->mTextureCoords[channel][candidate] !=
                            LerpVec(mesh->mTextureCoords[channel][va],
                                    mesh->mTextureCoords[channel][vb], t)) {
                return false;
            }
        }
        return true;
    };

    auto animStreamsMatch = [&](unsigned int candidate, unsigned int va, unsigned int vb,
                                    ai_real t) {
        for (unsigned int a = 0; a < mesh->mNumAnimMeshes; ++a) {
            const aiAnimMesh *anim = mesh->mAnimMeshes[a];
            if (nullptr == anim) {
                continue;
            }
            if (anim->mVertices &&
                    anim->mVertices[candidate] !=
                            LerpVec(anim->mVertices[va], anim->mVertices[vb], t)) {
                return false;
            }
            if (anim->mNormals &&
                    anim->mNormals[candidate] !=
                            LerpNormalized(anim->mNormals[va], anim->mNormals[vb], t)) {
                return false;
            }
            if (anim->mTangents &&
                    anim->mTangents[candidate] !=
                            LerpNormalized(anim->mTangents[va], anim->mTangents[vb], t)) {
                return false;
            }
            if (anim->mBitangents &&
                    anim->mBitangents[candidate] !=
                            LerpNormalized(anim->mBitangents[va], anim->mBitangents[vb], t)) {
                return false;
            }
            for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_COLOR_SETS; ++channel) {
                if (anim->mColors[channel] &&
                        anim->mColors[channel][candidate] !=
                                LerpColor(anim->mColors[channel][va],
                                        anim->mColors[channel][vb], t)) {
                    return false;
                }
            }
            for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++channel) {
                if (anim->mTextureCoords[channel] &&
                        anim->mTextureCoords[channel][candidate] !=
                                LerpVec(anim->mTextureCoords[channel][va],
                                        anim->mTextureCoords[channel][vb], t)) {
                    return false;
                }
            }
        }
        return true;
    };

    // Decide reuse vs. new vertex for every split point and grow all
    // streams. One logical split point creates exactly one vertex per
    // indexed edge, even when several verbose-format indices share it.
    for (auto &tableEntry : edgeTable) {
        EdgeInfo &info = tableEntry.second;
        for (size_t slot = 0; slot < info.splits.size(); ++slot) {
            const SplitPoint &split = info.splits[slot];
            const unsigned int primaryCandidate = split.vertex;
            const ai_real t = split.parameter;

            // The original index is reused only when every equivalent
            // candidate carries exactly the interpolated base shape,
            // anim streams and bone weights.
            bool reuse = true;
            for (unsigned int candidate : split.equivalents) {
                if (!baseStreamMatches(candidate, info.low, info.high, t) ||
                        !animStreamsMatch(candidate, info.low, info.high, t)) {
                    reuse = false;
                    break;
                }
                if (mesh->mNumBones) {
                    const WeightMap expected = LerpWeightMap(
                            originalWeights[info.low], originalWeights[info.high], t);
                    WeightMap expectedNonZero;
                    for (const auto &w : expected) {
                        if (w.second != ai_real(0.0)) {
                            expectedNonZero.insert(w);
                        }
                    }
                    if (expectedNonZero != originalWeights[candidate]) {
                        reuse = false;
                        break;
                    }
                }
            }

            if (reuse) {
                info.nodesLowToHigh[slot + 1] = primaryCandidate;
            } else {
                info.nodesLowToHigh[slot + 1] =
                        AppendInterpolatedVertex(mesh, info, t);
            }
        }
    }

    // Rewrite the faces: expand every edge into its boundary sub-segments
    // and triangulate the resulting simple planar rings.
    unsigned int outFaceCount = 0;
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices < 3) {
            ++outFaceCount;
        } else {
            unsigned int ringSize = 0;
            for (unsigned int k = 0; k < face.mNumIndices; ++k) {
                const EdgeKey key = MakeEdgeKey(
                        face.mIndices[k], face.mIndices[(k + 1) % face.mNumIndices]);
                const auto found = edgeTable.find(key);
                ringSize += (found == edgeTable.end())
                        ? 1u
                        : static_cast<unsigned int>(found->second.nodesLowToHigh.size() - 1);
            }
            outFaceCount += ringSize - 2;
        }
    }

    aiFace *outFaces = new aiFace[outFaceCount]();
    unsigned int outCursor = 0;
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices < 3) {
            // Points and lines pass through unchanged, keeping their buffer.
            aiFace &copied = outFaces[outCursor++];
            copied.mNumIndices = face.mNumIndices;
            copied.mIndices = face.mIndices;
            face.mIndices = nullptr;
            continue;
        }

        std::vector<unsigned int> ring;
        ring.reserve(face.mNumIndices * 2);
        for (unsigned int k = 0; k < face.mNumIndices; ++k) {
            const unsigned int start = face.mIndices[k];
            const unsigned int end = face.mIndices[(k + 1) % face.mNumIndices];
            const EdgeKey key = MakeEdgeKey(start, end);
            if (k == 0) {
                ring.push_back(start);
            }
            const auto found = edgeTable.find(key);
            if (found == edgeTable.end()) {
                if (k + 1 < face.mNumIndices) {
                    ring.push_back(end); // closing edge ends at the first ring node
                }
                continue;
            }
            const EdgeInfo &info = found->second;
            if (start == info.low) {
                for (size_t slot = 1; slot + 1 < info.nodesLowToHigh.size(); ++slot) {
                    ring.push_back(info.nodesLowToHigh[slot]);
                }
            } else {
                for (size_t slot = info.nodesLowToHigh.size() - 1; slot > 1; --slot) {
                    ring.push_back(info.nodesLowToHigh[slot - 1]);
                }
            }
            if (k + 1 < face.mNumIndices) {
                ring.push_back(end); // closing edge ends at the first ring node
            }
        }

        const std::vector<aiFace> triangles = TriangulateRing(mesh, ring);
        for (const aiFace &triangle : triangles) {
            aiFace &dst = outFaces[outCursor++];
            dst.mNumIndices = 3;
            dst.mIndices = new unsigned int[3];
            dst.mIndices[0] = triangle.mIndices[0];
            dst.mIndices[1] = triangle.mIndices[1];
            dst.mIndices[2] = triangle.mIndices[2];
        }
        delete[] face.mIndices;
        face.mIndices = nullptr;
    }

    delete[] mesh->mFaces;
    mesh->mFaces = outFaces;
    mesh->mNumFaces = outCursor;

    mesh->mPrimitiveTypes |= aiPrimitiveType_TRIANGLE;
    mesh->mPrimitiveTypes &= ~aiPrimitiveType_POLYGON;
    // The ear-clipped output is not NGON fan encoded.
    mesh->mPrimitiveTypes &= ~aiPrimitiveType_NGONEncodingFlag;

    return true;
}

// ------------------------------------------------------------------------------------------------
std::vector<aiFace> ResolveTJunctionsProcess::TriangulateRing(
        const aiMesh *mesh, const std::vector<unsigned int> &ring) {
    std::vector<aiFace> faces;
    const size_t count = ring.size();
    if (count < 3) {
        return faces;
    }

    std::vector<aiVector3D> points(count);
    for (size_t i = 0; i < count; ++i) {
        points[i] = mesh->mVertices[ring[i]];
    }

    // Newell normal of the planar ring.
    aiVector3D normal(0.0, 0.0, 0.0);
    for (size_t i = 0; i < count; ++i) {
        const aiVector3D &a = points[i];
        const aiVector3D &b = points[(i + 1) % count];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }

    const ai_real ax = std::abs(normal.x);
    const ai_real ay = std::abs(normal.y);
    const ai_real az = std::abs(normal.z);
    unsigned int axisA = 0;
    unsigned int axisB = 1;
    if (ax > ay) {
        if (ax > az) {
            axisA = 1;
            axisB = 2;
        }
    } else if (ay > az) {
        axisA = 2;
        axisB = 0;
    }
    if (normal[axisA] < 0.0) {
        std::swap(axisA, axisB);
    }

    const std::vector<std::array<unsigned int, 3>> triangles =
            EarClipRing(points, axisA, axisB);
    faces.reserve(triangles.size());
    for (const auto &tri : triangles) {
        aiFace face;
        face.mNumIndices = 3;
        face.mIndices = new unsigned int[3];
        face.mIndices[0] = ring[tri[0]];
        face.mIndices[1] = ring[tri[1]];
        face.mIndices[2] = ring[tri[2]];
        faces.push_back(face);
    }
    return faces;
}

// ------------------------------------------------------------------------------------------------
bool ResolveTJunctionsProcess::ResolveMesh(aiMesh *mesh, ai_real eps) {
    if (!IsValidEpsilon(eps)) {
        return false;
    }
    std::map<EdgeKey, EdgeSplit> splits = CollectEdgeSplits(mesh, eps);
    if (splits.empty()) {
        return false;
    }
    return ApplyToMesh(mesh, splits);
}

#endif // !! ASSIMP_BUILD_NO_TRIANGULATE_PROCESS
