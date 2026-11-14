//****************************************************************************//
// submesh.cpp                                                                //
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

#include "cal3d/submesh.h"
#include "cal3d/error.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coresubmorphtarget.h"

#include <string.h>
using namespace cal3d;
// For Exclusive type morph targets, we record a replacement attenuation after
// encountering the first Replace blend.  Until then, we recognize that we do
// not yet have a value by setting this field to this specific invalid value.
static float const ReplacementAttenuationNull = 100.0; // Any number not between zero and one.

CalSubmesh::CalSubmesh(CalCoreSubmesh *coreSubmesh)
{
    assert(coreSubmesh);

    m_pCoreSubmesh = coreSubmesh;

    // reserve memory for the face vector
    m_vectorFace.reserve(m_pCoreSubmesh->getFaceCount());
    m_vectorFace.resize(m_pCoreSubmesh->getFaceCount());

    // set the initial lod level
    setLodLevel(1.0f);

    // set the initial material id
    m_coreMaterialId = -1;

    //Setting the morph target weights
    m_vectorMorphTargetWeight.reserve(m_pCoreSubmesh->getCoreSubMorphTargetCount());
    m_vectorMorphTargetWeight.resize(m_pCoreSubmesh->getCoreSubMorphTargetCount());
    m_vectorAccumulatedWeight.reserve(m_pCoreSubmesh->getCoreSubMorphTargetCount());
    m_vectorAccumulatedWeight.resize(m_pCoreSubmesh->getCoreSubMorphTargetCount());
    m_vectorReplacementAttenuation.reserve(m_pCoreSubmesh->getCoreSubMorphTargetCount());
    m_vectorReplacementAttenuation.resize(m_pCoreSubmesh->getCoreSubMorphTargetCount());

    // Array is indexed by group, and there can't be more groups than there are morph targets.
    m_vectorSubMorphTargetGroupAttenuator.reserve(m_pCoreSubmesh->getCoreSubMorphTargetCount());
    m_vectorSubMorphTargetGroupAttenuator.resize(m_pCoreSubmesh->getCoreSubMorphTargetCount());
    m_vectorSubMorphTargetGroupAttenuation.reserve(m_pCoreSubmesh->getCoreSubMorphTargetCount());
    m_vectorSubMorphTargetGroupAttenuation.resize(m_pCoreSubmesh->getCoreSubMorphTargetCount());

    int morphTargetId;
    for(morphTargetId = 0; morphTargetId<m_pCoreSubmesh->getCoreSubMorphTargetCount(); ++morphTargetId)
    {
        m_vectorSubMorphTargetGroupAttenuator[ morphTargetId ] = -1; // No attenuator by default.
        m_vectorSubMorphTargetGroupAttenuation[ morphTargetId ] = 0.0f; // No attenuation by default.
        m_vectorMorphTargetWeight[morphTargetId] = 0.0f;
        m_vectorAccumulatedWeight[morphTargetId] = 0.0f;
        m_vectorReplacementAttenuation[morphTargetId] = ReplacementAttenuationNull;
    }

    // check if the submesh instance must handle the vertex and normal data internally
    if(m_pCoreSubmesh->getSpringCount() > 0)
    {
        m_vectorVertex.reserve(m_pCoreSubmesh->getVertexCount());
        m_vectorVertex.resize(m_pCoreSubmesh->getVertexCount());
        m_vectorNormal.reserve(m_pCoreSubmesh->getVertexCount());
        m_vectorNormal.resize(m_pCoreSubmesh->getVertexCount());

        m_vectorvectorTangentSpace.reserve(m_pCoreSubmesh->getVectorVectorTangentSpace().size());
        m_vectorvectorTangentSpace.resize(m_pCoreSubmesh->getVectorVectorTangentSpace().size());

        m_vectorPhysicalProperty.reserve(m_pCoreSubmesh->getVertexCount());
        m_vectorPhysicalProperty.resize(m_pCoreSubmesh->getVertexCount());

        // get the vertex vector of the core submesh
        std::vector<CalCoreSubmesh::Vertex>& vectorVertex = m_pCoreSubmesh->getVectorVertex();

        // copy the data from the core submesh as default values
        int vertexId;
        for(vertexId = 0; vertexId < m_pCoreSubmesh->getVertexCount(); ++vertexId)
        {
            // copy the vertex data
            m_vectorVertex[vertexId] = vectorVertex[vertexId].position;
            m_vectorPhysicalProperty[vertexId].position = vectorVertex[vertexId].position;
            m_vectorPhysicalProperty[vertexId].positionOld = vectorVertex[vertexId].position;

            // copy the normal data
            m_vectorNormal[vertexId] = vectorVertex[vertexId].normal;
        }

        m_bInternalData = true;
    }
    else
    {
        m_bInternalData = false;
    }

    // Every instance owns its own local morph results, sized to the full
    // core vertex set independently of the internal/spring data arrays.
    int coreVertexCount = m_pCoreSubmesh->getVertexCount();
    m_vectorMorphedVertex.reserve(coreVertexCount);
    m_vectorMorphedVertex.resize(coreVertexCount);
    m_vectorMorphedNormal.reserve(coreVertexCount);
    m_vectorMorphedNormal.resize(coreVertexCount);
    m_vectorLastMorphTargetWeight.reserve(m_pCoreSubmesh->getCoreSubMorphTargetCount());
    m_vectorLastMorphTargetWeight.resize(m_pCoreSubmesh->getCoreSubMorphTargetCount(), 0.0f);
    m_vectorMorphVertexDirty.reserve(coreVertexCount);
    m_vectorMorphVertexDirty.resize(coreVertexCount, 0);
    for(int vertexId = 0; vertexId < coreVertexCount; ++vertexId)
    {
        m_vectorMorphedVertex[vertexId] = m_pCoreSubmesh->getVectorVertex()[vertexId].position;
        m_vectorMorphedNormal[vertexId] = m_pCoreSubmesh->getVectorVertex()[vertexId].normal;
    }

    // The core data is immutable from here on, so the sparse index is
    // built exactly once and never revisited at runtime.
    buildMorphDeltaIndex();
}

/*****************************************************************************/
/** Builds the sparse morph delta index.
  *
  * Initialization is allowed to scan the whole core data set.  For every
  * target we record the vertices that actually carry a delta, and for every
  * vertex the target delta pairs touching it.  Only non-identical targets
  * create a pair, so a fully unchanged target keeps an empty support set and
  * is still preserved as a target (its zero deltas cost nothing at runtime).
  *****************************************************************************/

void CalSubmesh::buildMorphDeltaIndex()
{
    int morphTargetCount = m_pCoreSubmesh->getCoreSubMorphTargetCount();
    int coreVertexCount = m_pCoreSubmesh->getVertexCount();

    m_vectorMorphTargetVertices.clear();
    m_vectorMorphTargetVertices.resize(morphTargetCount);
    m_vectorVertexMorphDelta.clear();
    m_vectorVertexMorphDelta.resize(coreVertexCount);

    std::vector<CalCoreSubMorphTarget*>& vectorSubMorphTarget =
        m_pCoreSubmesh->getVectorCoreSubMorphTarget();
    std::vector<CalCoreSubmesh::Vertex>& vectorVertex =
        m_pCoreSubmesh->getVectorVertex();

    for(int morphTargetId = 0; morphTargetId < morphTargetCount; ++morphTargetId)
    {
        CalCoreSubMorphTarget* morphTarget = vectorSubMorphTarget[morphTargetId];
        int blendVertexCount = morphTarget->getBlendVertexCount();
        if(blendVertexCount > coreVertexCount)
        {
            blendVertexCount = coreVertexCount;
        }

        for(int vertexId = 0; vertexId < blendVertexCount; ++vertexId)
        {
            const CalCoreSubMorphTarget::BlendVertex* blendVertex =
                morphTarget->getBlendVertex(vertexId);
            if(!blendVertex)
            {
                continue;
            }

            // BlendVertex holds absolute targets; a vertex identical to the
            // base shape contributes no delta in either position or normal.
            const CalCoreSubmesh::Vertex& baseVertex = vectorVertex[vertexId];
            bool hasPositionDelta = (blendVertex->position != baseVertex.position);
            bool hasNormalDelta   = (blendVertex->normal   != baseVertex.normal);
            if(!hasPositionDelta && !hasNormalDelta)
            {
                continue;
            }

            MorphDeltaRef deltaRef;
            deltaRef.morphTargetId = morphTargetId;
            deltaRef.blendVertex = blendVertex;

            m_vectorVertexMorphDelta[vertexId].push_back(deltaRef);
            m_vectorMorphTargetVertices[morphTargetId].push_back(vertexId);
        }
    }
}

/*****************************************************************************/
/** Evaluates the local morph results incrementally.
  *
  * If no weight changed since the last evaluation this is an O(target count)
  * comparison with zero delta accesses and zero rebuilt vertices.  Otherwise
  * only the union of the support sets of the changed targets is rebuilt, each
  * dirty vertex being recomputed from the base shape and the current weights,
  * so toggling weights back and forth cannot accumulate drift.  Bone-space
  * coordinates never enter these arrays.
  *****************************************************************************/

void CalSubmesh::syncMorphCache()
{
    int morphTargetCount = (int)m_vectorMorphTargetWeight.size();

    // Identify targets whose weight changed, allowing one full comparison.
    m_vectorMorphDirtyVertex.clear();
    bool anyChanged = false;
    for(int morphTargetId = 0; morphTargetId < morphTargetCount; ++morphTargetId)
    {
        float weight = m_vectorMorphTargetWeight[morphTargetId];
        if(weight == m_vectorLastMorphTargetWeight[morphTargetId])
        {
            continue;
        }
        anyChanged = true;

        const std::vector<int>& touchedVertices =
            m_vectorMorphTargetVertices[morphTargetId];
        for(size_t i = 0; i < touchedVertices.size(); ++i)
        {
            int vertexId = touchedVertices[i];
            if(!m_vectorMorphVertexDirty[vertexId])
            {
                m_vectorMorphVertexDirty[vertexId] = 1;
                m_vectorMorphDirtyVertex.push_back(vertexId);
            }
        }
    }

    m_morphWorkStats = MorphWorkStats();

    if(!anyChanged)
    {
        // Weights match the last evaluation: no morph work at all.
        return;
    }

    std::vector<CalCoreSubmesh::Vertex>& vectorVertex =
        m_pCoreSubmesh->getVectorVertex();

    size_t deltaVisits = 0;

    // Rebuild every dirty vertex straight from the base shape.
    for(size_t dirtyIndex = 0; dirtyIndex < m_vectorMorphDirtyVertex.size(); ++dirtyIndex)
    {
        int vertexId = m_vectorMorphDirtyVertex[dirtyIndex];
        m_vectorMorphVertexDirty[vertexId] = 0;

        const CalCoreSubmesh::Vertex& baseVertex = vectorVertex[vertexId];
        CalVector position = baseVertex.position;
        CalVector normal = baseVertex.normal;

        // Only this vertex's associated delta pairs are ever touched.
        const std::vector<MorphDeltaRef>& deltas =
            m_vectorVertexMorphDelta[vertexId];
        for(size_t i = 0; i < deltas.size(); ++i)
        {
            const MorphDeltaRef& deltaRef = deltas[i];
            float weight = m_vectorMorphTargetWeight[deltaRef.morphTargetId];
            if(weight == 0.0f)
            {
                continue;
            }
            const CalCoreSubMorphTarget::BlendVertex& blendVertex =
                *deltaRef.blendVertex;
            // Absolute targets: local = base + sum(w * (target - base)).
            position.x += weight * (blendVertex.position.x - baseVertex.position.x);
            position.y += weight * (blendVertex.position.y - baseVertex.position.y);
            position.z += weight * (blendVertex.position.z - baseVertex.position.z);
            normal.x   += weight * (blendVertex.normal.x   - baseVertex.normal.x);
            normal.y   += weight * (blendVertex.normal.y   - baseVertex.normal.y);
            normal.z   += weight * (blendVertex.normal.z   - baseVertex.normal.z);
            ++deltaVisits;
        }

        m_vectorMorphedVertex[vertexId] = position;
        m_vectorMorphedNormal[vertexId] = normal;
    }

    m_morphWorkStats.rebuiltVertices = m_vectorMorphDirtyVertex.size();
    m_morphWorkStats.deltaVisits = deltaVisits;

    // Snapshot the weights these results were computed against.
    for(int morphTargetId = 0; morphTargetId < morphTargetCount; ++morphTargetId)
    {
        m_vectorLastMorphTargetWeight[morphTargetId] =
            m_vectorMorphTargetWeight[morphTargetId];
    }
}



/*****************************************************************************/
/** Provides access to the face data.
  *
  * This function returns the face data (vertex indices) of the submesh
  * instance. The LOD setting of the submesh instance is taken into account.
  *
  * @param pFaceBuffer A pointer to the user-provided buffer where the face
  *                    data is written to.
  *
  * @return The number of faces written to the buffer.
  *****************************************************************************/
int CalSubmesh::getFaces(CalIndex *pFaceBuffer) const
{
    // copy the face vector to the face buffer
    memcpy(pFaceBuffer, &m_vectorFace[0], m_faceCount * sizeof(Face));

    return m_faceCount;
}

/*****************************************************************************/
/** Disable internal data (and thus springs system)
  *
  *****************************************************************************/

void CalSubmesh::disableInternalData()
{
    if(m_bInternalData)
    {
        m_vectorVertex.clear();
        m_vectorNormal.clear();
        m_vectorvectorTangentSpace.clear();
        m_vectorPhysicalProperty.clear();
        m_bInternalData=false;
    }

}

/*****************************************************************************/
/** Returns true if tangent vectors are enabled.
  *
  * This function returns true if the submesh contains tangent vectors.
  *
  * @return True if tangent vectors are enabled.
  *****************************************************************************/

bool CalSubmesh::isTangentsEnabled(int mapId) const
{
    return m_pCoreSubmesh->isTangentsEnabled(mapId);
}

/*****************************************************************************/
/** Enables (and calculates) or disables the storage of tangent spaces.
  *
  * This function enables or disables the storage of tangent space bases.
  *****************************************************************************/

bool CalSubmesh::enableTangents(int mapId, bool enabled)
{
    if(!m_pCoreSubmesh->enableTangents(mapId,enabled))
        return false;

    if(!m_bInternalData)
        return true;

    if(!enabled)
    {
        m_vectorvectorTangentSpace[mapId].clear();
        return true;
    }

    m_vectorvectorTangentSpace[mapId].reserve(m_pCoreSubmesh->getVertexCount());
    m_vectorvectorTangentSpace[mapId].resize(m_pCoreSubmesh->getVertexCount());

    // get the tangent space vector of the core submesh
    std::vector<CalCoreSubmesh::TangentSpace >& vectorTangentSpace = m_pCoreSubmesh->getVectorVectorTangentSpace()[mapId];

    // copy the data from the core submesh as default values
    int vertexId;
    for(vertexId = 0; vertexId < m_pCoreSubmesh->getVertexCount(); vertexId++)
    {
        // copy the tangent space data
        m_vectorvectorTangentSpace[mapId][vertexId].tangent=vectorTangentSpace[vertexId].tangent;
        m_vectorvectorTangentSpace[mapId][vertexId].crossFactor=vectorTangentSpace[vertexId].crossFactor;
    }

    return true;
}

/*****************************************************************************/
/** Sets the LOD level.
  *
  * This function sets the LOD level of the submesh instance.
  *
  * @param lodLevel The LOD level in the range [0.0, 1.0].
  *****************************************************************************/

void CalSubmesh::setLodLevel(float lodLevel)
{
    // clamp the lod level to [0.0, 1.0]
    if(lodLevel < 0.0f) lodLevel = 0.0f;
    if(lodLevel > 1.0f) lodLevel = 1.0f;

    // get the lod count of the core submesh
    int lodCount;
    lodCount = m_pCoreSubmesh->getLodCount();

    // calculate the target lod count
    lodCount = (int)((1.0f - lodLevel) * lodCount);

    // get vertex vector of the core submesh
    std::vector<CalCoreSubmesh::Vertex>& vectorVertex = m_pCoreSubmesh->getVectorVertex();
    int coreVertexCount = vectorVertex.size();
    const CalCoreSubmesh::Vertex*	coreVertexPtr = &vectorVertex[0];

    // calculate the new number of vertices
    m_vertexCount = coreVertexCount - lodCount;

    // get face vector of the core submesh
    std::vector<CalCoreSubmesh::Face>& vectorFace = m_pCoreSubmesh->getVectorFace();
    const CalCoreSubmesh::Face* coreFacePtr = &vectorFace[0];

    // calculate the new number of faces
    m_faceCount = vectorFace.size();

    int vertexId;
    for(vertexId = coreVertexCount - 1; vertexId >= m_vertexCount; --vertexId)
    {
        m_faceCount -= coreVertexPtr[vertexId].faceCollapseCount;
    }

    // fill the face vector with the collapsed vertex ids
    int faceId;
    Face*	myFacePtr = &m_vectorFace[0];
    for(faceId = 0; faceId < m_faceCount; ++faceId)
    {
        int vertexId;
        for(vertexId = 0; vertexId < 3; ++vertexId)
        {
            // get the vertex id
            CalIndex collapsedVertexId;
            collapsedVertexId = coreFacePtr[faceId].vertexId[vertexId];

            // collapse the vertex id until it fits into the current lod level
            while(collapsedVertexId >= m_vertexCount)
                collapsedVertexId = coreVertexPtr[collapsedVertexId].collapseId;

            // store the collapse vertex id in the submesh face vector
            myFacePtr[faceId].vertexId[vertexId] = collapsedVertexId;
        }
    }
}
  

//****************************************************************************//
