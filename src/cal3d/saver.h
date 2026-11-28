//****************************************************************************//
// saver.h                                                                    //
// Copyright (C) 2001, 2002 Bruno 'Beosil' Heidelberger                       //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#pragma once
#include "cal3d/BoostToStd.h"

#include "cal3d/global.h"

class CalCoreSkeleton;
class CalCoreBone;
class CalCoreAnimation;
class CalCoreMorphAnimation;
class CalCoreTrack;
class CalCoreKeyframe;
class CalCoreMorphTrack;
class CalCoreMorphKeyframe;
class CalCoreMesh;
class CalCoreSubmesh;
class CalCoreMaterial;
class CalVector;
class CalQuaternion;

class CAL3D_API CalSaver {
public:
    static std::string saveCoreAnimationToBuffer(ISTD::shared_ptr<CalCoreAnimation> pCoreAnimation);
    static std::string saveCoreMorphAnimationToBuffer(ISTD::shared_ptr<CalCoreMorphAnimation> pCoreMorphAnimation);
    static std::string saveCoreMaterialToBuffer(ISTD::shared_ptr<CalCoreMaterial> pCoreMaterial);
    static std::string saveCoreMeshToBuffer(ISTD::shared_ptr<CalCoreMesh> pCoreMesh);
    static std::string saveCoreSkeletonToBuffer(ISTD::shared_ptr<CalCoreSkeleton> pCoreSkeleton);

    static std::string saveCoreAnimationXmlToBuffer(ISTD::shared_ptr<CalCoreAnimation> pCoreAnimation);

    static bool saveCoreAnimation(const std::string& strFilename, CalCoreAnimation* pCoreAnimation);
    static bool saveCoreAnimation(std::ostream& stream, CalCoreAnimation* coreAnimation);

    static bool saveCoreMorphAnimation(const std::string& strFilename, CalCoreMorphAnimation* pCoreMorphAnimation);
    static bool saveCoreMorphAnimation(std::ostream& file, CalCoreMorphAnimation* pCoreMorphAnimation);

    static std::string saveCoreMorphAnimationXmlToBuffer(ISTD::shared_ptr<CalCoreMorphAnimation> pCoreMorphAnimation);

    static bool saveCoreMaterial(const std::string& strFilename, CalCoreMaterial* pCoreMaterial);
    static bool saveCoreMaterial(std::ostream& file, CalCoreMaterial* pCoreMaterial);

    static bool saveCoreMesh(const std::string& strFilename, CalCoreMesh* pCoreMesh);
    static bool saveCoreMesh(std::ostream& os, CalCoreMesh* pCoreMesh);

    static bool saveCoreSkeleton(const std::string& strFilename, CalCoreSkeleton* pCoreSkeleton);
    static bool saveCoreSkeleton(std::ostream& os, CalCoreSkeleton* pCoreSkeleton);

    static bool saveXmlCoreSkeleton(const std::string& strFilename, CalCoreSkeleton* pCoreSkeleton);
    static bool saveXmlCoreAnimation(const std::string& strFilename, CalCoreAnimation* pCoreAnimation);
    static bool saveXmlCoreAnimation(std::ostream& os, CalCoreAnimation* pCoreAnimation);
    static bool saveXmlCoreMorphAnimation(const std::string& strFilename, CalCoreMorphAnimation* pCoreMorphAnimation);
    static bool saveXmlCoreMorphAnimation(std::ostream& os, CalCoreMorphAnimation* pCoreMorphAnimation);
    static bool saveXmlCoreMesh(const std::string& strFilename, CalCoreMesh* pCoreMesh);
    static bool saveXmlCoreMesh(std::ostream& os, CalCoreMesh* pCoreMesh);
    static bool saveXmlCoreMaterial(const std::string& strFilename, CalCoreMaterial* pCoreMaterial);

private:
    static bool saveCoreBone(std::ostream& file, const CalCoreSkeleton* coreSkeleton, const CalCoreBone* pCoreBone);
    static bool saveCoreKeyframe(std::ostream& file, const std::string& strFilename, const CalCoreKeyframe* pCoreKeyframe);
    static bool saveCoreSubmesh(std::ostream& file, CalCoreSubmesh* pCoreSubmesh);
    static bool saveCoreTrack(std::ostream& file, const std::string& strFilename, CalCoreTrack* pCoreTrack);
    static bool saveCoreMorphKeyframe(std::ostream& file, const std::string& strFilename, CalCoreMorphKeyframe* pCoreMorphKeyframe);
    static bool saveCoreMorphTrack(std::ostream& file, const std::string& strFilename, CalCoreMorphTrack* pCoreMorphTrack);

private:
    CalSaver();
    ~CalSaver();
};
