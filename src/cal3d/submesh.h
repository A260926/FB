//****************************************************************************//
// submesh.h                                                                  //
// Copyright (C) 2001, 2002 Bruno 'Beosil' Heidelberger                       //
// Copyright (C) 2012, 2017 Julien 'MpEbUtCh3r' Valentin                      //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifndef CAL_SUBMESH_H
#define CAL_SUBMESH_H


#include "cal3d/global.h"
#include "cal3d/vector.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coresubmorphtarget.h"
#include <cstddef>

namespace cal3d{
	class CalCoreSubmesh;
	class CalCoreSubMorphTarget;


	// Structure used to return an array of the morphs that have non-zero weights.
	struct MorphIdAndWeight
	{
		int morphId_;
		float weight_;
	};

	// Actual work performed by the most recent sparse morph evaluation.
	// Counters only measure the local morph pass; skinning never adds to them.
	struct MorphWorkStats
	{
		// Number of vertices whose local morphed position/normal was rebuilt.
		std::size_t rebuiltVertices;
		// Number of sparse target/vertex delta pairs that were read. A pair
		// carrying both a position and a normal delta counts once.
		std::size_t deltaVisits;

		MorphWorkStats() : rebuiltVertices(0), deltaVisits(0) { }
	};
	class CAL3D_API CalSubmesh
	{
	public:
		struct PhysicalProperty
		{
			CalVector position;
			CalVector positionOld;
			CalVector force;
		};

		struct TangentSpace
		{
			CalVector tangent;
			float crossFactor;
		};

		struct Face
		{
			CalIndex vertexId[3];
		};

	public:
		CalSubmesh(CalCoreSubmesh *coreSubmesh);
		~CalSubmesh() { }

		/**return coresubmesh model of the submesh**/
		inline CalCoreSubmesh *getCoreSubmesh()						{ return m_pCoreSubmesh; }
		inline const CalCoreSubmesh *getCoreSubmesh() const			{ return m_pCoreSubmesh; }

		/**get the number of vertices of this sub mesh.**/
		inline int getVertexCount() const							{ return m_vertexCount; }

		/**get the position vector.**/
		inline std::vector<CalVector>& getVectorVertex()			{ return m_vectorVertex; }
		/**get the position vector.**/
		inline const std::vector<CalVector>& getVectorVertex() const{ return m_vectorVertex; }

		/**return normal vector**/
		inline std::vector<CalVector>& getVectorNormal()			{ return m_vectorNormal; }
		/**return normal vector**/
		inline const std::vector<CalVector>& getVectorNormal() const{ return m_vectorNormal; }

		/**return tangent space vector vector**/
		inline std::vector<std::vector<TangentSpace> >& getVectorVectorTangentSpace()				{ return m_vectorvectorTangentSpace; }
		/**return tangent space vector vector**/
		inline  const std::vector<std::vector<TangentSpace> >& getVectorVectorTangentSpace() const	{ return m_vectorvectorTangentSpace; }

		/** return the physical property vector**/
		inline std::vector<PhysicalProperty>& getVectorPhysicalProperty()							{ return m_vectorPhysicalProperty; }
		/** return the physical property vector**/
		inline const std::vector<PhysicalProperty>& getVectorPhysicalProperty() const				{ return m_vectorPhysicalProperty; }

		/** return if tangent vectors are enabled.*/
		bool isTangentsEnabled(int mapId) const;
		/**Enables (and calculates) or disables the storage of tangent spaces.**/
		bool enableTangents(int mapId, bool enabled);

		/**get the core material ID of the submesh instance**/
		inline int getCoreMaterialId() const						{ return m_coreMaterialId; }
		/**set the core material ID of the submesh instance**/
		inline void setCoreMaterialId(int coreMaterialId)			{ m_coreMaterialId = coreMaterialId; }

		/**Sets the LOD level.**/
		void setLodLevel(float lodLevel);

		/**return number of faces of the submesh**/
		inline int getFaceCount() const								{ return m_faceCount; }
		/** returns the face data (vertex indices) of the submesh instance.
		*The LOD setting of the submesh instance is taken into account.
		* @param pFaceBuffer A pointer to the user-provided buffer where the face data is written to.**/
		int getFaces(CalIndex *pFaceBuffer) const;

		/**Set weight of a morph target with the given id.
		* @param blendId The morph target id.
		* @param weight The weight to be set.**/
		inline void setMorphTargetWeight(int blendId, float weight){ m_vectorMorphTargetWeight[blendId] = weight; }
		/**Get weight of a morph target with the given id.
		* @param blendId The morph target id.
		* @return The weight of the morph target.**/
		inline float getMorphTargetWeight(int blendId) const		{ return m_vectorMorphTargetWeight[blendId]; }

		/**returns the size of morph target weights vector**/
		inline int getMorphTargetWeightCount() const				{ return m_vectorMorphTargetWeight.size(); }
		/**returns the morph target weights vector**/
		inline std::vector<float>& getVectorMorphTargetWeight()		{ return m_vectorMorphTargetWeight; }
		/**returns the morph target weights vector**/
		inline const std::vector<float>& getVectorMorphTargetWeight() const{ return m_vectorMorphTargetWeight; }

		/** Recomputes the local morph results if the weights changed since the
		  * last evaluation.  Every skinning entry point calls this first, so
		  * weights written through setMorphTargetWeight, the MorphTargetMixer or
		  * the writable weight vector reference all take effect immediately.
		  * Only vertices touched by a target whose weight changed are rebuilt,
		  * and only their associated sparse delta pairs are read. **/
		void syncMorphCache();

		/** Local (pre-skinning) morphed positions, base + sum(w*(target-base)). **/
		inline const std::vector<CalVector>& getVectorMorphedVertex() const	{ return m_vectorMorphedVertex; }
		/** Local (pre-skinning) morphed normals, base + sum(w*(target-base)). **/
		inline const std::vector<CalVector>& getVectorMorphedNormal() const	{ return m_vectorMorphedNormal; }

		/** Work statistics of the most recent output entry point. **/
		inline MorphWorkStats getMorphWorkStats() const						{ return m_morphWorkStats; }

		/**returns wheter the submesh instance handles vertex data internally.**/
		inline bool hasInternalData() const							{ return m_bInternalData; }
		/** Disable internal data (and thus springs system)**/
		void disableInternalData();

	private:
		// One sparse target/vertex delta pair. BlendVertex stores absolute
		// targets, so the local result applies (target - base) scaled by weight.
		struct MorphDeltaRef
		{
			int												morphTargetId;
			const CalCoreSubMorphTarget::BlendVertex*		blendVertex;
		};

		// Builds the sparse delta index once; safe to scan all core data here.
		void buildMorphDeltaIndex();

		CalCoreSubmesh                         *m_pCoreSubmesh;
		std::vector<float>                      m_vectorMorphTargetWeight;
		std::vector<float>                      m_vectorAccumulatedWeight;
		std::vector<float>                      m_vectorReplacementAttenuation;
		std::vector<CalVector>                  m_vectorVertex;
		std::vector<CalVector>                  m_vectorNormal;
		std::vector<std::vector<TangentSpace> > m_vectorvectorTangentSpace;
		std::vector<Face>                       m_vectorFace;
		std::vector<PhysicalProperty>           m_vectorPhysicalProperty;
		std::vector<int>                        m_vectorSubMorphTargetGroupAttenuator;
		std::vector<float>                      m_vectorSubMorphTargetGroupAttenuation;
		int                                     m_vertexCount;
		int                                     m_faceCount;
		int                                     m_coreMaterialId;
		bool                                    m_bInternalData;

		// Sparse, immutable morph index built once in the constructor.
		// Vertices touched by each target, and targets touching each vertex.
		std::vector<std::vector<int> >          m_vectorMorphTargetVertices;
		std::vector<std::vector<MorphDeltaRef> >m_vectorVertexMorphDelta;

		// Per-instance local morph results (never share, never hold skinned data).
		std::vector<CalVector>                  m_vectorMorphedVertex;
		std::vector<CalVector>                  m_vectorMorphedNormal;

		// Weights used for the current local results; cache is rebuilt against these.
		std::vector<float>                      m_vectorLastMorphTargetWeight;

		// Reused scratch storage for incremental rebuilds.
		std::vector<char>                       m_vectorMorphVertexDirty;
		std::vector<int>                        m_vectorMorphDirtyVertex;

		MorphWorkStats                          m_morphWorkStats;
	};
}
#endif
