//****************************************************************************//
// physiquedualquaternion.cpp                                                 //
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

// This class implements a method described in the paper
// "Dual Quaternions for Rigid Transformation Blending"
// by Kavan, Collins, O'Sullivan, and Zara, 2006.

//****************************************************************************//
// Includes                                                                   //
//****************************************************************************//

#include "cal3d/error.h"
#include "cal3d/coremodel.h"
#include "cal3d/model.h"
#include "cal3d/mesh.h"
#include "cal3d/submesh.h"
#include "cal3d/skeleton.h"
#include "cal3d/bone.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coresubmorphtarget.h"
#include "cal3d/physiquedualquaternion.h"
#include "cal3d/dualquaternion.h"

#include <cfloat>
#include <numeric>


using namespace cal3d;

inline static void StoreVectorInBuffer( const CalVector& inVec, float* outBuffer )
{
  outBuffer[0] = inVec.x;
  outBuffer[1] = inVec.y;
  outBuffer[2] = inVec.z;
}

// Local morph blending now lives in CalSubmesh::syncMorphCache; these helpers
// only perform the dual-quaternion skinning of the already morphed data, so
// the LBS and DQ entries always start from identical local positions/normals.

static void CalcInfluencedPosition( const CalVector& morphedPosition,
                                    const std::vector<CalCoreSubmesh::Influence>& vectorInfluence,
                                    CalBone *const *vectorBone,
                                    CalVector& outPosition )
{
  // blend together all vertex influences
  size_t influenceCount = vectorInfluence.size();
  if (influenceCount == 0)
  {
    outPosition = morphedPosition;
  }
  else if (influenceCount == 1)
  {
    const CalBone*	oneBone = vectorBone[ vectorInfluence[0].boneId ];
    outPosition = morphedPosition;
    outPosition *= oneBone->getTransformMatrix();
    outPosition += oneBone->getTranslationBoneSpace();
  }
  else
  {
    CalDualQuaternion	blended;
    blended.nondual = CalQuaternion( 0.0f, 0.0f, 0.0f, 0.0f );
    blended.dual = CalQuaternion( 0.0f, 0.0f, 0.0f, 0.0f );
    CalQuaternion	pivot;

    for (size_t influenceId = 0; influenceId < influenceCount; ++influenceId)
    {
      // get the influence
      const CalCoreSubmesh::Influence& influence = vectorInfluence[influenceId];

      // get the bone of the influence vertex
      const CalBone *pBone = vectorBone[influence.boneId];

      // Get the dual quaternion for the bonespace transform
      CalDualQuaternion	boneTransform( pBone->getRotationBoneSpace(),
      	pBone->getTranslationBoneSpace() );

      // A dual quaternion and its negative both represent the same rigid
      // motion.  However, it can matter which one we use here.  If we
      // add two dual quaternions that are approximate negatives of each
      // other, we get a sum near zero, and then normalizing it becomes
      // numerically unstable.  So we try to avoid that case.
      if (influenceId == 0)
      {
        pivot = boneTransform.nondual;
      }
      else if (dot( boneTransform.nondual, pivot ) < 0.0f)
      {
        boneTransform *= -1.0f;
      }

      boneTransform *= influence.weight;

      blended += boneTransform;
    }

    blended.normalize();

    blended.transformPoint( morphedPosition, outPosition );
  }
}

static void CalcInfluencedNormal( const CalVector& morphedNormal,
                                  const std::vector<CalCoreSubmesh::Influence>& vectorInfluence,
                                  CalBone *const *vectorBone,
                                  CalVector& outNormal )
{
  // blend together all vertex influences
  size_t influenceCount = vectorInfluence.size();
  if (influenceCount == 0)
  {
    outNormal = morphedNormal;
  }
  else if (influenceCount == 1)
  {
    const CalBone*	oneBone = vectorBone[ vectorInfluence[0].boneId ];
    outNormal = morphedNormal;
    outNormal *= oneBone->getTransformMatrix();
  }
  else
  {
    CalDualQuaternion	blended;
    blended.nondual = CalQuaternion( 0.0f, 0.0f, 0.0f, 0.0f );
    blended.dual = CalQuaternion( 0.0f, 0.0f, 0.0f, 0.0f );
    CalQuaternion	pivot;

    for (size_t influenceId = 0; influenceId < influenceCount; ++influenceId)
    {
      // get the influence
      const CalCoreSubmesh::Influence& influence = vectorInfluence[influenceId];

      // get the bone of the influence vertex
      const CalBone *pBone = vectorBone[influence.boneId];

      // Get the dual quaternion for the rotation part of the bonespace
      // transform
      CalDualQuaternion	boneTransform( pBone->getRotationBoneSpace(),
        CalVector() );
      if (influenceId == 0)
      {
        pivot = boneTransform.nondual;
      }
      else if (dot( boneTransform.nondual, pivot ) < 0.0f)
      {
        boneTransform *= -1.0f;
      }
      boneTransform *= influence.weight;

      blended += boneTransform;
    }

    blended.transformPoint( morphedNormal, outNormal );
  }
}

 /*****************************************************************************/
/** Calculates the transformed vertex data.
  *
  * This function calculates and returns the transformed vertex data of a
  * specific submesh.
  *
  * @param pSubmesh A pointer to the submesh from which the vertex data should
  *                 be calculated and returned.
  * @param pVertexBuffer A pointer to the user-provided buffer where the vertex
  *                      data is written to.
  *
  * @return The number of vertices written to the buffer.
  *****************************************************************************/

int CalPhysiqueDualQuat::calculateVertices(CalSubmesh *pSubmesh, float *pVertexBuffer, int stride) const
{
  if (stride <= 0)
  {
    stride = 3*sizeof(float);
  }

  // get bone vector of the skeleton
  CalBone *const *vectorBone = &m_pModel->getSkeleton()->getVectorBone()[0];

  // get vertex vector of the core submesh
  const CalCoreSubmesh::Vertex* vectorVertex = &pSubmesh->getCoreSubmesh()->getVectorVertex()[0];

  // get physical property vector of the core submesh
  std::vector<CalCoreSubmesh::PhysicalProperty>& vectorPhysicalProperty =
    pSubmesh->getCoreSubmesh()->getVectorPhysicalProperty();

  // get the number of vertices
  const int vertexCount = pSubmesh->getVertexCount();

  // Evaluate the shared local morph cache once, identical to the LBS entries.
  pSubmesh->syncMorphCache();
  const std::vector<CalVector>& vectorMorphedVertex =
    pSubmesh->getVectorMorphedVertex();

  // Check for spring case
  bool hasSpringsAndInternalData =
    (pSubmesh->getCoreSubmesh()->getSpringCount() > 0) &&
    pSubmesh->hasInternalData();

  // calculate all submesh vertices
  int vertexId;
  for (vertexId = 0; vertexId < vertexCount; ++vertexId)
  {
    // get the vertex
    const CalCoreSubmesh::Vertex& vertex = vectorVertex[vertexId];

    // local morph blend shared by every skinning entry
    const CalVector& morphedPosition = vectorMorphedVertex[vertexId];

    // blend influences by bones
    CalVector	influencedPosition;
    CalcInfluencedPosition( morphedPosition, vertex.vectorInfluence,
      vectorBone, influencedPosition );
    influencedPosition.x *= m_axisFactorX;
    influencedPosition.y *= m_axisFactorY;
    influencedPosition.z *= m_axisFactorZ;

    // save vertex position
    if (hasSpringsAndInternalData)
    {
      // get the physical property of the vertex
      CalCoreSubmesh::PhysicalProperty& physicalProperty = vectorPhysicalProperty[vertexId];

      // assign new vertex position if there is no vertex weight
      if (physicalProperty.weight == 0.0f)
      {
      	StoreVectorInBuffer( influencedPosition, pVertexBuffer );
      }
    }
    else
    {
      StoreVectorInBuffer( influencedPosition, pVertexBuffer );
    }

    // next vertex position in buffer
    pVertexBuffer = (float *)(((char *)pVertexBuffer) + stride) ;
  }

  return vertexCount;
}


 /*****************************************************************************/
/** Calculates the transformed normal data.
  *
  * This function calculates and returns the transformed normal data of a
  * specific submesh.
  *
  * @param pSubmesh A pointer to the submesh from which the normal data should
  *                 be calculated and returned.
  * @param pNormalBuffer A pointer to the user-provided buffer where the normal
  *                      data is written to.
  *
  * @return The number of normals written to the buffer.
  *****************************************************************************/

int CalPhysiqueDualQuat::calculateNormals(CalSubmesh *pSubmesh, float *pNormalBuffer, int stride) const
{
  if(stride <= 0)
  {
    stride = 3*sizeof(float);
  }

  // get bone vector of the skeleton
  CalBone *const *vectorBone = &m_pModel->getSkeleton()->getVectorBone()[0];

  // get vertex vector of the submesh
  const CalCoreSubmesh::Vertex *vectorVertex = &pSubmesh->getCoreSubmesh()->getVectorVertex()[0];

  // get the number of vertices
  const int vertexCount = pSubmesh->getVertexCount();

  // same local morph cache as the LBS entries
  pSubmesh->syncMorphCache();
  const std::vector<CalVector>& vectorMorphedNormal =
    pSubmesh->getVectorMorphedNormal();

  // calculate normal for all submesh vertices
  int vertexId;
  for (vertexId = 0; vertexId < vertexCount; ++vertexId)
  {
    // get the vertex
    const CalCoreSubmesh::Vertex& vertex = vectorVertex[vertexId];

    // local morph blend shared by every skinning entry
    const CalVector& morphedNormal = vectorMorphedNormal[vertexId];

    // blend together all vertex influences
    CalVector	influencedNormal;
    CalcInfluencedNormal( morphedNormal, vertex.vectorInfluence,
      vectorBone, influencedNormal );

    // Normalization off keeps the transformed normal's length; on, normalize
    // every non-zero result and leave a zero normal at zero.
    if (m_Normalize)
    {
      influencedNormal.x /= m_axisFactorX;
      influencedNormal.y /= m_axisFactorY;
      influencedNormal.z /= m_axisFactorZ;
      float squaredLength = influencedNormal.x * influencedNormal.x
                          + influencedNormal.y * influencedNormal.y
                          + influencedNormal.z * influencedNormal.z;
      if(squaredLength > 0.0f)
      {
        influencedNormal.normalize();
      }
    }

    StoreVectorInBuffer( influencedNormal, pNormalBuffer );

    // next vertex position in buffer
    pNormalBuffer = (float *)(((char *)pNormalBuffer) + stride) ;
  }

  return vertexCount;
}

 /*****************************************************************************/
/** Calculates the transformed vertex data.
  *
  * This function calculates and returns the transformed vertex and the transformed 
  * normal datadata of a specific submesh.
  *
  * @param pSubmesh A pointer to the submesh from which the vertex data should
  *                 be calculated and returned.
  * @param pVertexBuffer A pointer to the user-provided buffer where the vertex
  *                      data is written to.
  *
  * @return The number of vertices written to the buffer.
  *****************************************************************************/

int CalPhysiqueDualQuat::calculateVerticesAndNormals(CalSubmesh *pSubmesh, float *pVertexBuffer, int stride) const
{
  if(stride <= 0)
  {
    stride = 6*sizeof(float);
  }

  // get bone vector of the skeleton
  CalBone *const *vectorBone = &m_pModel->getSkeleton()->getVectorBone()[0];

  // get vertex vector of the core submesh
  const CalCoreSubmesh::Vertex *vectorVertex = &pSubmesh->getCoreSubmesh()->getVectorVertex()[0];

  // get physical property vector of the core submesh
  std::vector<CalCoreSubmesh::PhysicalProperty>& vectorPhysicalProperty = pSubmesh->getCoreSubmesh()->getVectorPhysicalProperty();

  // get the number of vertices
  const int vertexCount = pSubmesh->getVertexCount();

  // same local morph cache as the LBS entries
  pSubmesh->syncMorphCache();
  const std::vector<CalVector>& vectorMorphedVertex =
    pSubmesh->getVectorMorphedVertex();
  const std::vector<CalVector>& vectorMorphedNormal =
    pSubmesh->getVectorMorphedNormal();

  // Check for spring case
  bool hasSpringsAndInternalData =
    (pSubmesh->getCoreSubmesh()->getSpringCount() > 0) &&
    pSubmesh->hasInternalData();

  // calculate all submesh vertices
  int vertexId;
  for(vertexId = 0; vertexId < vertexCount; ++vertexId)
  {
    // get the vertex
    const CalCoreSubmesh::Vertex& vertex = vectorVertex[vertexId];

    // local morph blend shared by every skinning entry
    const CalVector& morphedPosition = vectorMorphedVertex[vertexId];
    const CalVector& morphedNormal   = vectorMorphedNormal[vertexId];

    // blend influences by bones
    CalVector	influencedPosition;
    CalcInfluencedPosition( morphedPosition, vertex.vectorInfluence,
      vectorBone, influencedPosition );
    influencedPosition.x *= m_axisFactorX;
    influencedPosition.y *= m_axisFactorY;
    influencedPosition.z *= m_axisFactorZ;

    CalVector	influencedNormal;
    CalcInfluencedNormal( morphedNormal, vertex.vectorInfluence,
      vectorBone, influencedNormal );

    // Normalization off keeps the transformed normal's length; on, normalize
    // every non-zero result and leave a zero normal at zero.
    if (m_Normalize)
    {
      influencedNormal.x /= m_axisFactorX;
      influencedNormal.y /= m_axisFactorY;
      influencedNormal.z /= m_axisFactorZ;
      float squaredLength = influencedNormal.x * influencedNormal.x
                          + influencedNormal.y * influencedNormal.y
                          + influencedNormal.z * influencedNormal.z;
      if(squaredLength > 0.0f)
      {
        influencedNormal.normalize();
      }
    }

    // save vertex position
    if(hasSpringsAndInternalData)
    {
      // get the physical property of the vertex
      CalCoreSubmesh::PhysicalProperty& physicalProperty = vectorPhysicalProperty[vertexId];

      // assign new vertex position if there is no vertex weight
      if(physicalProperty.weight == 0.0f)
      {
        StoreVectorInBuffer( influencedPosition, pVertexBuffer );
      }
    }
    else
    {
      StoreVectorInBuffer( influencedPosition, pVertexBuffer );
    }

    pVertexBuffer[3] = influencedNormal.x;
    pVertexBuffer[4] = influencedNormal.y;
    pVertexBuffer[5] = influencedNormal.z;

    // next vertex position in buffer
    pVertexBuffer = (float *)(((char *)pVertexBuffer) + stride) ;
  }

  return vertexCount;
}

 /*****************************************************************************/
/** Calculates the transformed vertex data.
  *
  * This function calculates and returns the transformed vertex, the transformed 
  * normal data and the texture coords of a specific submesh.
  *
  * @param pSubmesh A pointer to the submesh from which the vertex data should
  *                 be calculated and returned.
  * 
  * @param pVertexBuffer A pointer to the user-provided buffer where the vertex
  *                      data is written to.
  *
  * @param NumTexCoords A integer with the number of texture coords
  *
  * @return The number of vertices written to the buffer.
  *****************************************************************************/


int CalPhysiqueDualQuat::calculateVerticesNormalsAndTexCoords(CalSubmesh *pSubmesh, float *pVertexBuffer, int NumTexCoords) const
{
  // get bone vector of the skeleton
  CalBone *const *vectorBone = &m_pModel->getSkeleton()->getVectorBone()[0];

  // get vertex vector of the core submesh
  const CalCoreSubmesh::Vertex *vectorVertex = &pSubmesh->getCoreSubmesh()->getVectorVertex()[0];

  // get the texture coordinate vector vector
  std::vector<std::vector<CalCoreSubmesh::TextureCoordinate> >& vectorvectorTextureCoordinate = pSubmesh->getCoreSubmesh()->getVectorVectorTextureCoordinate();

  int TextureCoordinateCount=(int)vectorvectorTextureCoordinate.size();

  // check if the map id is valid
  if(((NumTexCoords < 0) || (NumTexCoords > TextureCoordinateCount)))
  {
    if(TextureCoordinateCount!=0)
    {    
      CalError::setLastError(CalError::INVALID_HANDLE, __FILE__, __LINE__);
      return -1;
    }
  }

  // get physical property vector of the core submesh
  std::vector<CalCoreSubmesh::PhysicalProperty>& vectorPhysicalProperty = pSubmesh->getCoreSubmesh()->getVectorPhysicalProperty();

  // get the number of vertices
  const int vertexCount = pSubmesh->getVertexCount();

  // same local morph cache as the LBS entries
  pSubmesh->syncMorphCache();
  const std::vector<CalVector>& vectorMorphedVertex =
    pSubmesh->getVectorMorphedVertex();
  const std::vector<CalVector>& vectorMorphedNormal =
    pSubmesh->getVectorMorphedNormal();

  // Check for spring case
  bool hasSpringsAndInternalData =
    (pSubmesh->getCoreSubmesh()->getSpringCount() > 0) &&
    pSubmesh->hasInternalData();

  // calculate all submesh vertices
  int vertexId;
  for(vertexId = 0; vertexId < vertexCount; ++vertexId)
  {
    // get the vertex
    const CalCoreSubmesh::Vertex& vertex = vectorVertex[vertexId];

    // local morph blend shared by every skinning entry
    const CalVector& morphedPosition = vectorMorphedVertex[vertexId];
    const CalVector& morphedNormal   = vectorMorphedNormal[vertexId];

    // blend influences by bones
    CalVector	influencedPosition;
    CalcInfluencedPosition( morphedPosition, vertex.vectorInfluence,
      vectorBone, influencedPosition );
    influencedPosition.x *= m_axisFactorX;
    influencedPosition.y *= m_axisFactorY;
    influencedPosition.z *= m_axisFactorZ;

    CalVector	influencedNormal;
    CalcInfluencedNormal( morphedNormal, vertex.vectorInfluence,
      vectorBone, influencedNormal );

    // Normalization off keeps the transformed normal's length; on, normalize
    // every non-zero result and leave a zero normal at zero.
    if (m_Normalize)
    {
      influencedNormal.x /= m_axisFactorX;
      influencedNormal.y /= m_axisFactorY;
      influencedNormal.z /= m_axisFactorZ;
      float squaredLength = influencedNormal.x * influencedNormal.x
                          + influencedNormal.y * influencedNormal.y
                          + influencedNormal.z * influencedNormal.z;
      if(squaredLength > 0.0f)
      {
        influencedNormal.normalize();
      }
    }

    // save vertex position
    if(hasSpringsAndInternalData)
    {
      // get the physical property of the vertex
      CalCoreSubmesh::PhysicalProperty& physicalProperty = vectorPhysicalProperty[vertexId];

      // assign new vertex position if there is no vertex weight
      if(physicalProperty.weight == 0.0f)
      {
        StoreVectorInBuffer( influencedPosition, pVertexBuffer );
      }
    }
    else
    {
      StoreVectorInBuffer( influencedPosition, pVertexBuffer );
    }

    pVertexBuffer[3] = influencedNormal.x;
    pVertexBuffer[4] = influencedNormal.y;
    pVertexBuffer[5] = influencedNormal.z;

    // next vertex position in buffer	
    pVertexBuffer += 6;

    if(TextureCoordinateCount==0)
    {
      pVertexBuffer+=NumTexCoords*2;
    }
    else
    {
      for(int mapId=0; mapId < NumTexCoords; ++mapId)
      {
        pVertexBuffer[0] = vectorvectorTextureCoordinate[mapId][vertexId].u;
        pVertexBuffer[1] = vectorvectorTextureCoordinate[mapId][vertexId].v;			
        pVertexBuffer += 2;
      }
    }
  }

  return vertexCount;
}
