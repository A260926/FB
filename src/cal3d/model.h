//****************************************************************************//
// model.h                                                                    //
// Copyright (C) 2001, 2002 Bruno 'Beosil' Heidelberger                       //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifndef CAL_MODEL_H
#define CAL_MODEL_H


#include "cal3d/global.h"
#include "cal3d/vector.h"

namespace cal3d{
	class CalCoreModel;
	class CalSkeleton;
	class CalAbstractMixer;
	class CalMixer;
	class CalMorphTargetMixer;
	class CalPhysique;
	class CalSpringSystem;
	class CalRenderer;
	class CalMesh;
	class CalFrameTransaction;


	class CAL3D_API CalModel : NonCopyable
	{
	public:
		CalModel(CalCoreModel *pCoreModel);
		~CalModel();

		bool attachMesh(int coreMeshId);
		bool detachMesh(int coreMeshId);
		CalCoreModel *getCoreModel();
		const CalCoreModel *getCoreModel() const;
		CalMesh *getMesh(int coreMeshId);
		const CalMesh *getMesh(int coreMeshId) const;
		CalMixer *getMixer();
		const CalMixer *getMixer() const;
		const CalAbstractMixer *getAbstractMixer() const;
		void setAbstractMixer(CalAbstractMixer *pMixer);
		CalMorphTargetMixer *getMorphTargetMixer();
		const CalMorphTargetMixer *getMorphTargetMixer() const;
		CalPhysique *getPhysique();
		const CalPhysique *getPhysique() const;
		void setPhysique(CalPhysique *physique);
		CalRenderer *getRenderer();
		const CalRenderer *getRenderer() const;
		CalSkeleton *getSkeleton();
		const CalSkeleton *getSkeleton() const;
		CalSpringSystem *getSpringSystem();
		const CalSpringSystem *getSpringSystem() const;
		CalBoundingBox & getBoundingBox(bool precision = false);
		const Cal::UserData getUserData() const;
		std::vector<CalMesh *>& getVectorMesh();
		void setLodLevel(float lodLevel);
		void setMaterialSet(int setId);
		void setUserData(Cal::UserData userData);
		void update(float deltaTime);
		void disableInternalData();

		/** Enables/disables the per-model frame callback transaction. The
		  * switch is only accepted while no animation is active, no playback
		  * command is queued and no update is running.
		  * @return the mode in effect after the call. */
		bool setFrameCallbacksEnabled(bool enabled);
		/** Returns whether frame callback mode is enabled. */
		bool getFrameCallbacksEnabled() const;
		/** Internal: the frame transaction of this model (may be null). */
		CalFrameTransaction *getFrameTransaction();
		/** Internal: const access to the frame transaction of this model. */
		const CalFrameTransaction *getFrameTransaction() const;

	private:
		CalCoreModel          *m_pCoreModel;
		CalSkeleton           *m_pSkeleton;
		CalAbstractMixer      *m_pMixer;
		CalMorphTargetMixer   *m_pMorphTargetMixer;
		CalPhysique           *m_pPhysique;
		CalSpringSystem       *m_pSpringSystem;
		CalRenderer           *m_pRenderer;
		CalFrameTransaction   *m_pFrameTransaction;
		Cal::UserData          m_userData;
		std::vector<CalMesh *> m_vectorMesh;
		CalBoundingBox         m_boundingBox;
	};
}
#endif

//****************************************************************************//
