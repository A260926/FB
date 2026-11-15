//****************************************************************************//
// physique.cpp                                                               //
// Copyright (C) 2001, 2002 Bruno 'Beosil' Heidelberger                       //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#ifndef IMVU_NO_INTRINSICS
#include <xmmintrin.h>
#endif
#include "cal3d/error.h"
#include "cal3d/physique.h"
#include "cal3d/submesh.h"
#include "cal3d/skeleton.h"
#include "cal3d/bone.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coremorphtarget.h"
#include "cal3d/transform.h"

#ifdef _MSC_VER
#pragma optimize("t", on)
#endif

// calculateVerticesAndNormals_{x87,SSE_intrinsics,SSE} are:
/*
  SSE Optimized Skinning Without Normals or Tangents
  Copyright (C) 2005 Id Software, Inc.
  Written by J.M.P. van Waveren
  This code is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  This code is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
*/

CAL3D_FORCEINLINE void ScaleMatrix(BoneTransform& result, const BoneTransform& mat, const float s) {
    result.rowx.x = s * mat.rowx.x;
    result.rowx.y = s * mat.rowx.y;
    result.rowx.z = s * mat.rowx.z;
    result.rowx.w = s * mat.rowx.w;
    result.rowy.x = s * mat.rowy.x;
    result.rowy.y = s * mat.rowy.y;
    result.rowy.z = s * mat.rowy.z;
    result.rowy.w = s * mat.rowy.w;
    result.rowz.x = s * mat.rowz.x;
    result.rowz.y = s * mat.rowz.y;
    result.rowz.z = s * mat.rowz.z;
    result.rowz.w = s * mat.rowz.w;
}
CAL3D_FORCEINLINE void AddScaledMatrix(BoneTransform& result, const BoneTransform& mat, const float s) {
    result.rowx.x += s * mat.rowx.x;
    result.rowx.y += s * mat.rowx.y;
    result.rowx.z += s * mat.rowx.z;
    result.rowx.w += s * mat.rowx.w;
    result.rowy.x += s * mat.rowy.x;
    result.rowy.y += s * mat.rowy.y;
    result.rowy.z += s * mat.rowy.z;
    result.rowy.w += s * mat.rowy.w;
    result.rowz.x += s * mat.rowz.x;
    result.rowz.y += s * mat.rowz.y;
    result.rowz.z += s * mat.rowz.z;
    result.rowz.w += s * mat.rowz.w;
}
CAL3D_FORCEINLINE void TransformPoint(CalVector4& result, const BoneTransform& m, const CalBase4& v) {
    result.x = m.rowx.x * v.x + m.rowx.y * v.y + m.rowx.z * v.z + m.rowx.w;
    result.y = m.rowy.x * v.x + m.rowy.y * v.y + m.rowy.z * v.z + m.rowy.w;
    result.z = m.rowz.x * v.x + m.rowz.y * v.y + m.rowz.z * v.z + m.rowz.w;
}
CAL3D_FORCEINLINE void TransformVector(CalVector4& result, const BoneTransform& m, const CalBase4& v) {
    result.x = m.rowx.x * v.x + m.rowx.y * v.y + m.rowx.z * v.z;
    result.y = m.rowy.x * v.x + m.rowy.y * v.y + m.rowy.z * v.z;
    result.z = m.rowz.x * v.x + m.rowz.y * v.y + m.rowz.z * v.z;
}

void CalPhysique::calculateVerticesAndNormals_x87(
    const BoneTransform* boneTransforms,
    size_t vertexCount,
    const CalCoreSubmesh::Vertex* vertices,
    const CalCoreSubmesh::Influence* influences,
    CalVector4* output_vertex
) {

    BoneTransform total_transform;

    // calculate all submesh vertices
    while (vertexCount--) {
        ScaleMatrix(total_transform, boneTransforms[influences->boneId], influences->weight);

        while (!influences++->lastInfluenceForThisVertex) {
            AddScaledMatrix(total_transform, boneTransforms[influences->boneId], influences->weight);
        }

        TransformPoint(output_vertex[0], total_transform, vertices->position);
        TransformVector(output_vertex[1], total_transform, vertices->normal);
        ++vertices;
        output_vertex += 2;
    }
}


#ifndef IMVU_NO_INTRINSICS
void CalPhysique::calculateVerticesAndNormals_SSE_intrinsics(
    const BoneTransform* boneTransforms,
    size_t vertexCount,
    const CalCoreSubmesh::Vertex* vertices,
    const CalCoreSubmesh::Influence* influences,
    CalVector4* output_vertex
) {

    __m128 rowx;
    __m128 rowy;
    __m128 rowz;
    __m128 weight;

    // calculate all submesh vertices
    while (vertexCount--) {
        weight = _mm_load_ss(&influences->weight);
        weight = _mm_shuffle_ps(weight, weight, _MM_SHUFFLE(0, 0, 0, 0));

        const BoneTransform& bt = boneTransforms[influences->boneId];

        rowx = _mm_mul_ps(_mm_load_ps((const float*)&bt.rowx), weight);
        rowy = _mm_mul_ps(_mm_load_ps((const float*)&bt.rowy), weight);
        rowz = _mm_mul_ps(_mm_load_ps((const float*)&bt.rowz), weight);

        while (!influences++->lastInfluenceForThisVertex) {
            weight = _mm_load_ss(&influences->weight);
            weight = _mm_shuffle_ps(weight, weight, _MM_SHUFFLE(0, 0, 0, 0));

            const BoneTransform& bt = boneTransforms[influences->boneId];

            rowx = _mm_add_ps(rowx, _mm_mul_ps(_mm_load_ps((const float*)&bt.rowx), weight));
            rowy = _mm_add_ps(rowy, _mm_mul_ps(_mm_load_ps((const float*)&bt.rowy), weight));
            rowz = _mm_add_ps(rowz, _mm_mul_ps(_mm_load_ps((const float*)&bt.rowz), weight));
        }

        {
            // transform position
            const __m128 position = _mm_load_ps((const float*)&vertices->position);

            const __m128 mulx = _mm_mul_ps(position, rowx);
            const __m128 muly = _mm_mul_ps(position, rowy);
            const __m128 mulz = _mm_mul_ps(position, rowz);

            const __m128 copylo = _mm_unpacklo_ps(mulx, muly);
            const __m128 copyhi = _mm_unpackhi_ps(mulx, muly);
            const __m128 sum1 = _mm_add_ps(copylo, copyhi);

            const __m128 lhps = _mm_movelh_ps(mulz, sum1);
            const __m128 hlps = _mm_movehl_ps(sum1, mulz);
            const __m128 sum2 = _mm_add_ps(lhps, hlps);

            _mm_storeh_pi((__m64*)output_vertex, sum2);

            __m128 sum3 = _mm_shuffle_ps(sum2, sum2, _MM_SHUFFLE(1, 1, 1, 1));
            sum3 = _mm_add_ss(sum2, sum3);
            _mm_store_ss(&output_vertex->z, sum3);
        }

        // transform normal
        {
            const __m128 normal = _mm_load_ps((const float*)&vertices->normal);

            const __m128 mulx = _mm_mul_ps(normal, rowx);
            const __m128 muly = _mm_mul_ps(normal, rowy);
            const __m128 mulz = _mm_mul_ps(normal, rowz);

            const __m128 copylo = _mm_unpacklo_ps(mulx, muly);
            const __m128 copyhi = _mm_unpackhi_ps(mulx, muly);
            const __m128 sum1 = _mm_add_ps(copylo, copyhi);

            const __m128 lhps = _mm_movelh_ps(mulz, sum1);
            const __m128 hlps = _mm_movehl_ps(sum1, mulz);
            const __m128 sum2 = _mm_add_ps(lhps, hlps);

            _mm_storeh_pi((__m64*)&output_vertex[1], sum2);

            __m128 sum3 = _mm_shuffle_ps(sum2, sum2, _MM_SHUFFLE(1, 1, 1, 1));
            sum3 = _mm_add_ss(sum2, sum3);
            _mm_store_ss(&output_vertex[1].z, sum3);
        }

        ++vertices;
        output_vertex += 2;
    }
}
#endif

void automaticallyDetectSkinRoutine(
    const BoneTransform* boneTransforms,
    size_t vertexCount,
    const CalCoreSubmesh::Vertex* vertices,
    const CalCoreSubmesh::Influence* influences,
    CalVector4* output_vertices);

static CalPhysique::SkinRoutine optimizedSkinRoutine = automaticallyDetectSkinRoutine;

void automaticallyDetectSkinRoutine(
    const BoneTransform* boneTransforms,
    size_t vertexCount,
    const CalCoreSubmesh::Vertex* vertices,
    const CalCoreSubmesh::Influence* influences,
    CalVector4* output_vertices
) {

#ifdef IMVU_NO_INTRINSICS
    optimizedSkinRoutine = CalPhysique::calculateVerticesAndNormals_x87;
#else
    optimizedSkinRoutine = CalPhysique::calculateVerticesAndNormals_SSE_intrinsics;
#endif
    return optimizedSkinRoutine(boneTransforms, vertexCount, vertices, influences, output_vertices);
}

namespace {
    cal3d::SSEArray<CalCoreSubmesh::Vertex> MorphSubmeshCache;

    void accumulateMorphTarget(
        const cal3d::MorphTarget* morphTarget
    ) {
        // VC++ isn't hoisting this SSE register out of the loop, so do it manually.
        CalVector4 weight(morphTarget->weight);

        const CalCoreMorphTarget::VertexOffsetArray& vertexOffsets = morphTarget->coreMorphTarget->vertexOffsets;
        const VertexOffset* morphVertex = cal3d::pointerFromVector(vertexOffsets);
        const VertexOffset* lastMorphVertex = morphVertex + vertexOffsets.size();
        for (; morphVertex != lastMorphVertex; ++morphVertex) {
            size_t i = morphVertex->vertexId;
            MorphSubmeshCache[i].position += weight * morphVertex->position;
            MorphSubmeshCache[i].normal   += weight * morphVertex->normal;
        }
    }

    const CalCoreSubmesh::Vertex* accumulateMorphTargets(
        size_t vertexCount,
        const CalCoreSubmesh::Vertex* sourceVertices,
        const cal3d::MorphTarget* morphTarget,
        const cal3d::MorphTarget* morphTargetEnd
    ) {
        if (vertexCount > MorphSubmeshCache.size()) {
            MorphSubmeshCache.destructive_resize(vertexCount);
        }

        cal3d::verify(morphTarget->weight != 0.0f, "Don't bother accumulating morphs until you have found an active morph target");

        // In theory we could initialize the accumulation buffer with
        // "morphTarget", since we know it's active.  It would reduce
        // memory traffic but increase code complexity, and probably
        // never matters in practice.
        std::copy(sourceVertices, sourceVertices + vertexCount, MorphSubmeshCache.begin());

        // Now find active morph targets and accumulate them
        while (morphTarget != morphTargetEnd) {
            if (morphTarget->weight != 0.0f) {
                accumulateMorphTarget(morphTarget);
            }
            ++morphTarget;
        }

        return cal3d::pointerFromVector(MorphSubmeshCache);
    }
}

void CalPhysique::calculateVerticesAndNormals(
    const BoneTransform* boneTransforms,
    const CalSubmesh* submesh,
    float* pVertexBuffer
) {
    CalCoreSubmesh* coreSubmesh = submesh->coreSubmesh.get();
    const size_t vertexCount = coreSubmesh->getVertexCount();
    const CalCoreSubmesh::Vertex* sourceVertices = cal3d::pointerFromVector(coreSubmesh->getVectorVertex());

    // TODO: We could kill the O(PossibleMorphTargets) loop by maintaining
    // the set of active morph targets in CalSubmesh

    const cal3d::MorphTarget* morphTarget = cal3d::pointerFromVector(submesh->morphTargets);
    const cal3d::MorphTarget* morphTargetEnd = morphTarget + submesh->morphTargets.size();
    for (; morphTarget != morphTargetEnd; ++morphTarget) {
        if (morphTarget->weight != 0.0f) {
            // found one!
            // accumulate the morphed vertices in a temporary buffer and skin it
            sourceVertices = accumulateMorphTargets(vertexCount, sourceVertices, morphTarget, morphTargetEnd);
            break;
        }
    }

    return optimizedSkinRoutine(
        boneTransforms,
        vertexCount,
        sourceVertices,
        cal3d::pointerFromVector(coreSubmesh->getInfluences()),
        reinterpret_cast<CalVector4*>(pVertexBuffer));
}

#ifdef _MSC_VER
#pragma optimize("", on)
#endif
