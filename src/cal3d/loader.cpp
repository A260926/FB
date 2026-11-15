//****************************************************************************//
// loader.cpp                                                                 //
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

#include <stdexcept>
#include <stdint.h>
#include <rapidxml.hpp>
#include "cal3d/BoostToStd.h"
#include "cal3d/memory.h"
#include "cal3d/loader.h"
#include "cal3d/error.h"
#include "cal3d/vector.h"
#include "cal3d/quaternion.h"
#include "cal3d/coreskeleton.h"
#include "cal3d/corebone.h"
#include "cal3d/coreanimation.h"
#include "cal3d/coremorphanimation.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/coremesh.h"
#include "cal3d/coremorphtrack.h"
#include "cal3d/coremorphkeyframe.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coremorphtarget.h"
#include "cal3d/corematerial.h"
#include "cal3d/tinyxml.h"
#include "cal3d/buffersource.h"
#include "cal3d/xmlformat.h"

// Quat format:
//
//  axis selection (2),
//  asign (1), afixed (n),
//  bsign (1), bfixed (n),
//  csign (1), cfixed (n),
//  time (in 1/30th second steps)
unsigned int const CalLoader::keyframeBitsPerOriComponent = 11;
unsigned int const CalLoader::keyframeBitsPerTime = 10;
//unsigned int const keyframeTimeMax = ( 1 << CalLoader::keyframeBitsPerTime );

// My resolution is in 1/4 units which is 1/2mm if 1 unit is 2mm.
// My large range is 2^23 units, or 2^24mm, which is 2^4km, or 16km.
// My small range is 2^7 units, or 2^8mm, which is 2.56m.
// The large pos has X, Y, Z, having 25 bits plus a sign bit, totaling 78 bits, leaving two bits padding.
// The small pos has X, Y, Z, having 9 bits plus a sign bit, totaling 30 bits, leaving two bits padding.
unsigned int const CalLoader::keyframeBitsPerUnsignedPosComponent = 25;
unsigned int const CalLoader::keyframeBitsPerPosPadding = 2;
unsigned int const CalLoader::keyframePosBytes = 10;
float const CalLoader::keyframePosRange = (1 << (CalLoader::keyframeBitsPerUnsignedPosComponent - 2));     // In units.

unsigned int const CalLoader::keyframeBitsPerUnsignedPosComponentSmall = 9;
unsigned int const CalLoader::keyframeBitsPerPosPaddingSmall = 2;
unsigned int const CalLoader::keyframePosBytesSmall = 4;
float const CalLoader::keyframePosRangeSmall = (1 << (CalLoader::keyframeBitsPerUnsignedPosComponentSmall - 2));



template<typename T>
void allocateVectorWhereSizeIsGuarded(size_t n, std::vector<T> &o_ret, int lineNumMacroVal) {
    try {
        o_ret.resize(n);
    } catch (std::bad_alloc&) {
        CalError calerr;
        calerr.setLastError(CalError::MEMORY_ALLOCATION_FAILED, __FILE__, lineNumMacroVal);
        throw calerr;
    } catch (std::length_error&) {
        CalError calerr;
        calerr.setLastError(CalError::MEMORY_ALLOCATION_FAILED, __FILE__, lineNumMacroVal);
        throw calerr;
    }
}

bool CAL3D_API CalVectorFromDataSrc(CalBufferSource& dataSrc, CalVector* calVec) {
    return dataSrc.readFloat(calVec->x) &&
           dataSrc.readFloat(calVec->y) &&
           dataSrc.readFloat(calVec->z);
}

bool CAL3D_API CalVectorFromDataSrc(CalBufferSource& dataSrc, CalPoint4* calVec) {
    CalVector v;
    if (CalVectorFromDataSrc(dataSrc, &v)) {
        *calVec = CalPoint4(v);
        return true;
    } else {
        return false;
    }
}

bool CAL3D_API CalVectorFromDataSrc(CalBufferSource& dataSrc, CalVector4* calVec) {
    CalVector v;
    if (CalVectorFromDataSrc(dataSrc, &v)) {
        *calVec = CalVector4(v);
        return true;
    } else {
        return false;
    }
}

bool
TranslationWritten(CalCoreKeyframe* lastCoreKeyframe, bool translationRequired, bool translationIsDynamic) {
    return (translationRequired && (!lastCoreKeyframe || translationIsDynamic));
}


bool CalLoader::isHeaderWellFormed(const rapidxml::xml_node<>* node) {
    return node->first_attribute("magic", 0, false) && node->first_attribute("version", 0, false);
}

template<typename T>
struct ScopedArray {
    explicit ScopedArray(size_t size)
        : ptr(new T[size])
    {}

    ~ScopedArray() {
        delete[] ptr;
    }

    T* ptr;

private:
    ScopedArray();
    ScopedArray(const ScopedArray&);
    ScopedArray& operator=(const ScopedArray&);
};

struct ForceCLocale {
    ForceCLocale() {
#ifdef _WIN32
        previousThreadState = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
#endif
        previousLocale = setlocale(LC_ALL, "C");
    }

    ~ForceCLocale() {
        setlocale(LC_ALL, previousLocale);
#ifdef _WIN32
        _configthreadlocale(previousThreadState);
#endif
    }

private:
#ifdef _WIN32
    int previousThreadState;
#endif
    const char* previousLocale;
};

template<typename RV>
RV tryBothLoaders(
    CalBufferSource& inputSource,
    RV(*binaryLoader)(CalBufferSource&),
    RV(*xmlLoader)(char*)
) {
    try {
        if (RV anim = binaryLoader(inputSource)) {
            return anim;
        }
        // make a copy to null-terminate and allow the xml parser to modify memory in-place
        ScopedArray<char> data(inputSource.size() + 1);
        memcpy(data.ptr, inputSource.data(), inputSource.size());
        data.ptr[inputSource.size()] = 0;

        ForceCLocale fcl;
        return xmlLoader(data.ptr);
    } catch (const CalError&) {
        return RV();
    }
}

ISTD::shared_ptr<CalCoreAnimation> CalLoader::loadCoreAnimation(CalBufferSource& inputSrc) {
    return tryBothLoaders(inputSrc, &loadBinaryCoreAnimation, &loadXmlCoreAnimation);
}

ISTD::shared_ptr<CalCoreMorphAnimation> CalLoader::loadCoreMorphAnimation(CalBufferSource& inputSrc) {
    return tryBothLoaders(inputSrc, &loadBinaryCoreMorphAnimation, &loadXmlCoreMorphAnimation);
}

ISTD::shared_ptr<CalCoreMaterial> CalLoader::loadCoreMaterial(CalBufferSource& inputSrc) {
    return tryBothLoaders(inputSrc, &loadBinaryCoreMaterial, &loadXmlCoreMaterial);
}

ISTD::shared_ptr<CalCoreMesh> CalLoader::loadCoreMesh(CalBufferSource& inputSrc) {
    return tryBothLoaders(inputSrc, &loadBinaryCoreMesh, &loadXmlCoreMesh);
}

ISTD::shared_ptr<CalCoreSkeleton> CalLoader::loadCoreSkeleton(CalBufferSource& inputSrc) {
    return tryBothLoaders(inputSrc, &loadBinaryCoreSkeleton, &loadXmlCoreSkeleton);
}

ISTD::shared_ptr<CalCoreAnimation> CalLoader::loadBinaryCoreAnimation(CalBufferSource& dataSrc) {
    const ISTD::shared_ptr<CalCoreAnimation> null;

    // check if this is a valid file
    char magic[4];
    if (!dataSrc.readBytes(&magic[0], 4) || (memcmp(&magic[0], cal3d::ANIMATION_FILE_MAGIC, 4) != 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // check if the version is compatible with the library
    int version;
    if (!dataSrc.readInteger(version) || (version < cal3d::EARLIEST_COMPATIBLE_FILE_VERSION) || (version > cal3d::CURRENT_FILE_VERSION)) {
        CalError::setLastError(CalError::INCOMPATIBLE_FILE_VERSION, __FILE__, __LINE__);
        return null;
    }

    bool useAnimationCompression = CalLoader::usesAnimationCompression(version);
    if (cal3d::versionHasCompressionFlag(version)) {
        int compressionFlag = 0;
        if (!dataSrc.readInteger(compressionFlag)) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
            return null;
        }
        // Only really need the first bit.
        useAnimationCompression = (compressionFlag != 0);
    }


    ISTD::shared_ptr<CalCoreAnimation> pCoreAnimation = ISTD::make_shared<CalCoreAnimation>();

    float duration;
    if (!dataSrc.readFloat(duration)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    if (duration <= 0.0f) {
        CalError::setLastError(CalError::INVALID_ANIMATION_DURATION, __FILE__, __LINE__);
        return null;
    }

    // set the duration in the core animation instance
    if (duration != duration) {
        duration = 0;
    }
    pCoreAnimation->duration = duration;

    // read the number of tracks
    int trackCount;
    if (!dataSrc.readInteger(trackCount) || (trackCount <= 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // load all core bones
    for (int trackId = 0; trackId < trackCount; ++trackId) {
        // load the core track
        ISTD::shared_ptr<CalCoreTrack> pCoreTrack(loadCoreTrack(dataSrc, version, useAnimationCompression));
        if (!pCoreTrack) {
            return null;
        }

        // add the core track to the core animation instance
        pCoreAnimation->tracks.push_back(*pCoreTrack);
    }

    return pCoreAnimation;
}



ISTD::shared_ptr<CalCoreMorphAnimation> CalLoader::loadBinaryCoreMorphAnimation(CalBufferSource& dataSrc) {
    const ISTD::shared_ptr<CalCoreMorphAnimation> null;

    char magic[4];
    if (!dataSrc.readBytes(&magic[0], 4) || (memcmp(&magic[0], cal3d::ANIMATEDMORPH_FILE_MAGIC, 4) != 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // check if the version is compatible with the library
    int version;
    if (!dataSrc.readInteger(version) || (version < cal3d::EARLIEST_COMPATIBLE_FILE_VERSION) || (version > cal3d::CURRENT_FILE_VERSION)) {
        CalError::setLastError(CalError::INCOMPATIBLE_FILE_VERSION, __FILE__, __LINE__);
        return null;
    }

    // allocate a new core animatedMorph instance
    ISTD::shared_ptr<CalCoreMorphAnimation> pCoreMorphAnimation = ISTD::make_shared<CalCoreMorphAnimation>();

    // get the duration of the core animatedMorph
    float duration;
    if (!dataSrc.readFloat(duration)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // check for a valid duration
    if (duration <= 0.0f) {
        CalError::setLastError(CalError::INVALID_ANIMATION_DURATION, __FILE__, __LINE__);
        return null;
    }

    // set the duration in the core animatedMorph instance
    pCoreMorphAnimation->duration = duration;

    // read the number of tracks
    int trackCount;
    if (!dataSrc.readInteger(trackCount) || (trackCount <= 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    for (int trackId = 0; trackId < trackCount; ++trackId) {
        ISTD::shared_ptr<CalCoreMorphTrack> pCoreTrack = loadCoreMorphTrack(dataSrc);
        if (!pCoreTrack) {
            return null;
        }

        pCoreMorphAnimation->tracks.push_back(*pCoreTrack);
    }

    return pCoreMorphAnimation;
}


/*****************************************************************************/
/** Loads a core material instance.
 *
 * This function loads a core material instance from a data source.
 *
 * @param dataSrc The data source to load the core material instance from.
 *
 * @return One of the following values:
 *         \li a pointer to the core material
 *         \li \b 0 if an error happened
 *****************************************************************************/

ISTD::shared_ptr<CalCoreMaterial> CalLoader::loadBinaryCoreMaterial(CalBufferSource& dataSrc) {
    const ISTD::shared_ptr<CalCoreMaterial> null;

    // check if this is a valid file
    char magic[4];
    if (!dataSrc.readBytes(&magic[0], 4) || (memcmp(&magic[0], cal3d::MATERIAL_FILE_MAGIC, 4) != 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // check if the version is compatible with the library
    int version;
    if (!dataSrc.readInteger(version) || (version < cal3d::EARLIEST_COMPATIBLE_FILE_VERSION) || (version > cal3d::CURRENT_FILE_VERSION)) {
        CalError::setLastError(CalError::INCOMPATIBLE_FILE_VERSION, __FILE__, __LINE__);
        return null;
    }

    bool hasMaterialTypes = (version >= cal3d::FIRST_FILE_VERSION_WITH_MATERIAL_TYPES);

    // allocate a new core material instance
    ISTD::shared_ptr<CalCoreMaterial> pCoreMaterial = ISTD::make_shared<CalCoreMaterial>();

    // get the ambient color of the core material
    unsigned char ambientColor[4];
    if (!dataSrc.readBytes(&ambientColor, sizeof(ambientColor))) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // get the diffuse color of the core material
    unsigned char diffuseColor[4];
    if (!dataSrc.readBytes(&diffuseColor, sizeof(diffuseColor))) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // get the specular color of the core material
    unsigned char specularColor[4];
    if (!dataSrc.readBytes(&specularColor, sizeof(specularColor))) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // get the shininess factor of the core material
    float shininess;
    dataSrc.readFloat(shininess);

    // read the number of maps
    int mapCount;
    if (!dataSrc.readInteger(mapCount) || (mapCount < 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // load all maps
    for (int mapId = 0; mapId < mapCount; ++mapId) {
        CalCoreMaterial::Map map;

        if (!dataSrc.readString(map.filename)){
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
            return null;
        }

        if (hasMaterialTypes) {
            if (!dataSrc.readString(map.type)){
                CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
                return null;
            }
        } else {
            map.type = "";
        }

        pCoreMaterial->maps.push_back(map);
    }

    return pCoreMaterial;
}

/*****************************************************************************/
/** Loads a core mesh instance.
 *
 * This function loads a core mesh instance from a data source.
 *
 * @param dataSrc The data source to load the core mesh instance from.
 *
 * @return One of the following values:
 *         \li a pointer to the core mesh
 *         \li \b 0 if an error happened
 *****************************************************************************/

ISTD::shared_ptr<CalCoreMesh> CalLoader::loadBinaryCoreMesh(CalBufferSource& dataSrc) {
    const ISTD::shared_ptr<CalCoreMesh> null;

    // check if this is a valid file
    char magic[4];
    if (!dataSrc.readBytes(&magic[0], 4) || (memcmp(&magic[0], cal3d::MESH_FILE_MAGIC, 4) != 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // check if the version is compatible with the library
    int version;
    if (!dataSrc.readInteger(version) || (version < cal3d::EARLIEST_COMPATIBLE_FILE_VERSION) || (version > cal3d::CURRENT_FILE_VERSION)) {
        CalError::setLastError(CalError::INCOMPATIBLE_FILE_VERSION, __FILE__, __LINE__);
        return null;
    }

    // get the number of submeshes
    int submeshCount;
    if (!dataSrc.readInteger(submeshCount)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    ISTD::shared_ptr<CalCoreMesh> pCoreMesh = ISTD::make_shared<CalCoreMesh>();

    // load all core submeshes
    for (int submeshId = 0; submeshId < submeshCount; ++submeshId) {
        // load the core submesh
        ISTD::shared_ptr<CalCoreSubmesh> pCoreSubmesh(loadCoreSubmesh(dataSrc, version));
        if (!pCoreSubmesh) {
            return null;
        }

        // add the core submesh to the core mesh instance
        pCoreMesh->submeshes.push_back(pCoreSubmesh);
    }

    return pCoreMesh;
}

/*****************************************************************************/
/** Loads a core skeleton instance.
 *
 * This function loads a core skeleton instance from a data source.
 *
 * @param dataSrc The data source to load the core skeleton instance from.
 *
 * @return One of the following values:
 *         \li a pointer to the core skeleton
 *         \li \b 0 if an error happened
 *****************************************************************************/

ISTD::shared_ptr<CalCoreSkeleton> CalLoader::loadBinaryCoreSkeleton(CalBufferSource& dataSrc) {
    const ISTD::shared_ptr<CalCoreSkeleton> null;

    char magic[4];
    if (!dataSrc.readBytes(&magic[0], 4) || (memcmp(&magic[0], cal3d::SKELETON_FILE_MAGIC, 4) != 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // check if the version is compatible with the library
    int version;
    if (!dataSrc.readInteger(version) || (version < cal3d::EARLIEST_COMPATIBLE_FILE_VERSION) || (version > cal3d::CURRENT_FILE_VERSION)) {
        CalError::setLastError(CalError::INCOMPATIBLE_FILE_VERSION, __FILE__, __LINE__);
        return null;
    }

    bool hasNodeLights = (version >= cal3d::FIRST_FILE_VERSION_WITH_NODE_LIGHTS);

    // read the number of bones
    int boneCount;
    if (!dataSrc.readInteger(boneCount) || (boneCount < 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    ISTD::optional<CalVector> sceneAmbientColor;
    if (hasNodeLights) {
        CalVector sceneColor;
        CalVectorFromDataSrc(dataSrc, &sceneColor);
        sceneAmbientColor = sceneColor;
    }

    std::vector<ISTD::shared_ptr<CalCoreBone>> bones;

    for (int boneId = 0; boneId < boneCount; ++boneId) {
        ISTD::shared_ptr<CalCoreBone> pCoreBone(loadCoreBones(dataSrc, version));
        if (!pCoreBone) {
            return null;
        }

        bones.push_back(pCoreBone);
    }

    ISTD::shared_ptr<CalCoreSkeleton> pCoreSkeleton = ISTD::make_shared<CalCoreSkeleton>(bones);
    if (sceneAmbientColor) {
        pCoreSkeleton->sceneAmbientColor = *sceneAmbientColor;
    }
    return pCoreSkeleton;
}



/*****************************************************************************/
/** Loads a core bone instance.
 *
 * This function loads a core bone instance from a data source.
 *
 * @param dataSrc The data source to load the core bone instance from.
 *
 * @return One of the following values:
 *         \li a pointer to the core bone
 *         \li \b 0 if an error happened
 *****************************************************************************/

ISTD::shared_ptr<CalCoreBone> CalLoader::loadCoreBones(CalBufferSource& dataSrc, int version) {
    const ISTD::shared_ptr<CalCoreBone> null;

    bool hasNodeLights = (version >= cal3d::FIRST_FILE_VERSION_WITH_NODE_LIGHTS);

    // read the name of the bone
    std::string strName;
    dataSrc.readString(strName);

    // get the translation of the bone
    float tx, ty, tz;
    dataSrc.readFloat(tx);
    dataSrc.readFloat(ty);
    dataSrc.readFloat(tz);

    // get the rotation of the bone
    float rx, ry, rz, rw;
    dataSrc.readFloat(rx);
    dataSrc.readFloat(ry);
    dataSrc.readFloat(rz);
    dataSrc.readFloat(rw);

    // get the bone space translation of the bone
    float txBoneSpace, tyBoneSpace, tzBoneSpace;
    dataSrc.readFloat(txBoneSpace);
    dataSrc.readFloat(tyBoneSpace);
    dataSrc.readFloat(tzBoneSpace);

    // get the bone space rotation of the bone
    float rxBoneSpace, ryBoneSpace, rzBoneSpace, rwBoneSpace;
    dataSrc.readFloat(rxBoneSpace);
    dataSrc.readFloat(ryBoneSpace);
    dataSrc.readFloat(rzBoneSpace);
    dataSrc.readFloat(rwBoneSpace);

    // get the parent bone id
    int parentId;
    dataSrc.readInteger(parentId);

    // get the lgith type and light color
    int lightType = LIGHT_TYPE_NONE;
    CalVector lightColor;
    if (hasNodeLights) {
        dataSrc.readInteger(lightType);
        CalVectorFromDataSrc(dataSrc, &lightColor);
    }


    CalQuaternion rot(rx, ry, rz, -rw);
    CalQuaternion rotbs(rxBoneSpace, ryBoneSpace, rzBoneSpace, -rwBoneSpace);
    CalVector trans(tx, ty, tz);

    // allocate a new core bone instance
    ISTD::shared_ptr<CalCoreBone> pCoreBone = ISTD::make_shared<CalCoreBone>(strName, parentId);

    // set all attributes of the bone
    pCoreBone->relativeTransform.translation = trans;
    pCoreBone->relativeTransform.rotation = rot;
    pCoreBone->inverseBindPoseTransform.translation = CalVector(txBoneSpace, tyBoneSpace, tzBoneSpace);
    pCoreBone->inverseBindPoseTransform.rotation = rotbs;
    if (hasNodeLights) {
        pCoreBone->lightType = (CalLightType)lightType;
        pCoreBone->lightColor = lightColor;
    }

    // read the number of children
    int childCount;
    if (!dataSrc.readInteger(childCount) || (childCount < 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // load all children ids
    for (; childCount > 0; childCount--) {
        int childId;
        if (!dataSrc.readInteger(childId) || (childId < 0)) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
            return null;
        }
    }

    return pCoreBone;
}




bool
CalLoader::usesAnimationCompression(int version) {
    return (version >= cal3d::FIRST_FILE_VERSION_WITH_ANIMATION_COMPRESSION);
}


std::pair<bool, CalCoreKeyframe> CalLoader::loadCoreKeyframe(
    CalBufferSource& dataSrc,
    int version,
    CalCoreKeyframe* prevCoreKeyframe,
    bool translationRequired,
    bool highRangeRequired,
    bool translationIsDynamic,
    bool useAnimationCompression
) {
    float time;
    CalVector t;
    float rx, ry, rz, rw;

    if (useAnimationCompression) {
        unsigned int bytesRequired = compressedKeyframeRequiredBytes(prevCoreKeyframe, translationRequired, highRangeRequired, translationIsDynamic);
        assert(bytesRequired < 100);
        unsigned char buf[ 100 ];

        if (!dataSrc.readBytes(buf, bytesRequired)) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
            return std::pair<bool, CalCoreKeyframe>(false, CalCoreKeyframe());
        }

        CalVector vec;
        CalQuaternion quat;

        unsigned int bytesRead = readCompressedKeyframe(
            buf,
            &vec,
            &quat,
            &time,
            prevCoreKeyframe,
            translationRequired,
            highRangeRequired,
            translationIsDynamic
        );

        if (bytesRead != bytesRequired) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
            return std::pair<bool, CalCoreKeyframe>(false, CalCoreKeyframe());
        }

        t = vec;
        rx = quat.x;
        ry = quat.y;
        rz = quat.z;
        rw = quat.w;

        if (version < cal3d::FIRST_FILE_VERSION_WITH_ANIMATION_COMPRESSION6) {
            if (version >= cal3d::FIRST_FILE_VERSION_WITH_ANIMATION_COMPRESSION4) {
                if (version >= cal3d::FIRST_FILE_VERSION_WITH_ANIMATION_COMPRESSION5) {
                    if (TranslationWritten(prevCoreKeyframe, translationRequired, translationIsDynamic)) {
                        dataSrc.readFloat(t.x);
                        dataSrc.readFloat(t.y);
                        dataSrc.readFloat(t.z);
                    }
                }

                // get the rotation of the bone
                dataSrc.readFloat(rx);
                dataSrc.readFloat(ry);
                dataSrc.readFloat(rz);
                dataSrc.readFloat(rw);
            }
        }
    } else {
        dataSrc.readFloat(time);

        // get the translation of the bone
        dataSrc.readFloat(t.x);
        dataSrc.readFloat(t.y);
        dataSrc.readFloat(t.z);

        // get the rotation of the bone
        dataSrc.readFloat(rx);
        dataSrc.readFloat(ry);
        dataSrc.readFloat(rz);
        dataSrc.readFloat(rw);
    }

    return std::pair<bool, CalCoreKeyframe>(true, CalCoreKeyframe(time, t, CalQuaternion(rx, ry, rz, -rw)));
}


// Return the number of bytes required by the compressed binary format of a keyframe with these attributes.
unsigned int
CalLoader::compressedKeyframeRequiredBytes(CalCoreKeyframe* lastCoreKeyframe, bool translationRequired, bool highRangeRequired, bool translationIsDynamic) {
    unsigned int bytes = 0;
    if (translationRequired) {

        // If I am not the first keyframe in the track, and the translations are not dynamic (meaning
        // they are the same for all the keyframes in the track, though different from the skeleton), then
        // just use the translation from the last keyframe.
        if (lastCoreKeyframe && !translationIsDynamic) {
        } else {
            if (highRangeRequired) {
                bytes += keyframePosBytes;
            } else {
                bytes += keyframePosBytesSmall;
            }
        }
    }
    bytes += 6;
    return bytes;
}



static float const InvalidCoord = 1e10;
CalVector InvalidTranslation(InvalidCoord, InvalidCoord, InvalidCoord);

// Pass in the number of bytes that are valid.
// Returns number of byts read.
unsigned int
CalLoader::readCompressedKeyframe(
    unsigned char* buf,
    CalVector* vecResult, CalQuaternion* quatResult, float* timeResult,
    CalCoreKeyframe* lastCoreKeyframe,
    bool translationRequired, bool highRangeRequired, bool translationIsDynamic) {
    unsigned char* bufStart = buf;

    // Read in the translation or get it from the skeleton or zero it out as a last resort.
    if (translationRequired) {

        // If I am not the first keyframe in the track, and the translations are not dynamic (meaning
        // they are the same for all the keyframes in the track, though different from the skeleton), then
        // just use the translation from the last keyframe.
        if (lastCoreKeyframe && !translationIsDynamic) {
            * vecResult = lastCoreKeyframe->transform.translation;
        } else {
            unsigned int data;
            float tx, ty, tz;
            if (highRangeRequired) {
                BitReader br(buf);

                // Read x.
                br.read(& data, keyframeBitsPerUnsignedPosComponent);
                tx = FixedPointToFloatZeroToOne(data, keyframeBitsPerUnsignedPosComponent) * keyframePosRange;
                br.read(& data, 1);
                if (data) {
                    tx = - tx;
                }

                // Read y.
                br.read(& data, keyframeBitsPerUnsignedPosComponent);
                ty = FixedPointToFloatZeroToOne(data, keyframeBitsPerUnsignedPosComponent) * keyframePosRange;
                br.read(& data, 1);
                if (data) {
                    ty = - ty;
                }

                // Read z.
                br.read(& data, keyframeBitsPerUnsignedPosComponent);
                tz = FixedPointToFloatZeroToOne(data, keyframeBitsPerUnsignedPosComponent) * keyframePosRange;
                br.read(& data, 1);
                if (data) {
                    tz = - tz;
                }

                // Now even it off to n bytes.
                br.read(& data, keyframeBitsPerPosPadding);
                assert(br.bytesRead() == keyframePosBytes);
                buf += keyframePosBytes;
            } else {
                BitReader br(buf);

                // Read x.
                br.read(& data, keyframeBitsPerUnsignedPosComponentSmall);
                tx = FixedPointToFloatZeroToOne(data, keyframeBitsPerUnsignedPosComponentSmall) * keyframePosRangeSmall;
                br.read(& data, 1);
                if (data) {
                    tx = - tx;
                }

                // Read y.
                br.read(& data, keyframeBitsPerUnsignedPosComponentSmall);
                ty = FixedPointToFloatZeroToOne(data, keyframeBitsPerUnsignedPosComponentSmall) * keyframePosRangeSmall;
                br.read(& data, 1);
                if (data) {
                    ty = - ty;
                }

                // Read z.
                br.read(& data, keyframeBitsPerUnsignedPosComponentSmall);
                tz = FixedPointToFloatZeroToOne(data, keyframeBitsPerUnsignedPosComponentSmall) * keyframePosRangeSmall;
                br.read(& data, 1);
                if (data) {
                    tz = - tz;
                }

                // Now even it off to n bytes.
                br.read(& data, keyframeBitsPerPosPaddingSmall);
                assert(br.bytesRead() == keyframePosBytesSmall);
                buf += keyframePosBytesSmall;
            }
            vecResult->set(tx, ty, tz);
        }
    } else {
        *vecResult = InvalidTranslation;
    }

    // Read in the quat and time.
    float quat[ 4 ];
    unsigned int steps;
    unsigned int bytesRead = ReadQuatAndExtra(buf, quat, & steps, keyframeBitsPerOriComponent, keyframeBitsPerTime);
    buf += 6;

    (void)bytesRead;
    assert(bytesRead == 6);

    *quatResult = CalQuaternion(quat[ 0 ], quat[ 1 ], quat[ 2 ], quat[ 3 ]);
    *timeResult = steps / 30.0f;
    return buf - bufStart;
}



/*****************************************************************************/
/** Loads a core morphKeyframe instance.
 *
 * This function loads a core morphKeyframe instance from a data source.
 *
 * @param dataSrc The data source to load the core morphKeyframe instance from.
 *
 * @return One of the following values:
 *         \li a pointer to the core morphKeyframe
 *         \li \b 0 if an error happened
 *****************************************************************************/

CalCoreMorphKeyframe CalLoader::loadCoreMorphKeyframe(CalBufferSource& dataSrc) {
    CalCoreMorphKeyframe retFrame;
    // get the time of the morphKeyframe
    dataSrc.readFloat(retFrame.time);
    // get the translation of the bone
    dataSrc.readFloat(retFrame.weight);

    return retFrame;
}

// We may need this in other places in the future.
bool isValidUTF8(const std::string &str) {
    for (std::string::const_iterator iter = str.begin(); iter != str.end(); ++iter) {
        uint8_t c = *iter;
        int num;
        if ((c & 0x80) == 0x00) {
            // U+0000 to U+007F
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            // U+0080 to U+07FF 
            num = 2;
        } else if ((c & 0xF0) == 0xE0) {
            // U+0800 to U+FFFF 
            num = 3;
        } else if ((c & 0xF8) == 0xF0) {
            // U+10000 to U+10FFFF 
            num = 4;
        } else {
            return false;
        }

        for (int i = 1; i < num; ++i) {
            if (iter == str.end()) {
                return false;
            }

            ++iter;
            c = *iter;
            if ((c & 0xC0) != 0x80) {
                return false;
            }
        }
    }
    return true;
}

std::string convertLatin1toUTF8(const std::string &str) {
    std::string strOut;
    for (std::string::const_iterator iter = str.begin(); iter != str.end(); ++iter) {
        uint8_t c = *iter;
        if (c < 0x80) {
            strOut.push_back(c);
        }
        else {
            strOut.push_back(0xc0 | c >> 6);
            strOut.push_back(0x80 | (c & 0x3f));
        }
    }
    return strOut;
}


ISTD::shared_ptr<CalCoreSubmesh> CalLoader::loadCoreSubmesh(CalBufferSource& dataSrc, int version) {
    bool hasVertexColors = (version >= cal3d::FIRST_FILE_VERSION_WITH_VERTEX_COLORS);
    bool hasMorphTargetsInMorphFiles = (version >= cal3d::FIRST_FILE_VERSION_WITH_MORPH_TARGETS_IN_MORPH_FILES);

    // get the material thread id of the submesh
    int coreMaterialThreadId;
    dataSrc.readInteger(coreMaterialThreadId);

    // get the number of vertices, faces, level-of-details and springs
    int vertexCount;
    dataSrc.readInteger(vertexCount);

    int faceCount;
    dataSrc.readInteger(faceCount);

    int lodCount;
    dataSrc.readInteger(lodCount);

    int springCount;
    dataSrc.readInteger(springCount);

    // get the number of texture coordinates per vertex
    int textureCoordinateCount;
    dataSrc.readInteger(textureCoordinateCount);

    int morphCount = 0;
    if (hasMorphTargetsInMorphFiles) {
        dataSrc.readInteger(morphCount);
    }

    ISTD::shared_ptr<CalCoreSubmesh> pCoreSubmesh = ISTD::make_shared<CalCoreSubmesh>(vertexCount, textureCoordinateCount ? true : false, faceCount);
    pCoreSubmesh->coreMaterialThreadId = coreMaterialThreadId;

    for (int vertexId = 0; vertexId < vertexCount; ++vertexId) {
        CalCoreSubmesh::Vertex vertex;
        CalColor32 vertexColor;

        // load data of the vertex
        dataSrc.readFloat(vertex.position.x);
        dataSrc.readFloat(vertex.position.y);
        dataSrc.readFloat(vertex.position.z);
        dataSrc.readFloat(vertex.normal.x);
        dataSrc.readFloat(vertex.normal.y);
        dataSrc.readFloat(vertex.normal.z);
        vertexColor = CalMakeColor(CalVector4(1.0f, 1.0f, 1.0f, 1.0f));
        if (hasVertexColors) {
            CalVector4 vc;
            dataSrc.readFloat(vc.x);
            dataSrc.readFloat(vc.y);
            dataSrc.readFloat(vc.z);
            vc.w = 1.0f;
            vertexColor = CalMakeColor(vc);
        }
        int collapseId;
        int faceCollapseCount;
        dataSrc.readInteger(collapseId);
        dataSrc.readInteger(faceCollapseCount);

        for (int textureCoordinateId = 0; textureCoordinateId < textureCoordinateCount; ++textureCoordinateId) {
            CalCoreSubmesh::TextureCoordinate textureCoordinate;

            dataSrc.readFloat(textureCoordinate.u);
            dataSrc.readFloat(textureCoordinate.v);

            if (textureCoordinateId == 0) {
                pCoreSubmesh->setTextureCoordinate(vertexId, textureCoordinate);
            }
        }

        // get the number of influences
        int influenceCount;
        if (!dataSrc.readInteger(influenceCount) || (influenceCount < 0)) {
            return ISTD::shared_ptr<CalCoreSubmesh>();
        }

        std::vector<CalCoreSubmesh::Influence> influences;
        allocateVectorWhereSizeIsGuarded(influenceCount, influences, __LINE__);

        for (int influenceId = 0; influenceId < influenceCount; ++influenceId) {
            dataSrc.readInteger(influences[influenceId].boneId);
            dataSrc.readFloat(influences[influenceId].weight);
        }

        // set vertex in the core submesh instance
        pCoreSubmesh->addVertex(vertex, vertexColor, influences);

        // load the physical property of the vertex if there are springs in the core submesh
        if (springCount > 0) {
            float f;
            // load data of the physical property
            dataSrc.readFloat(f);
        }
    }

    // load all springs
    for (int springId = 0; springId < springCount; ++springId) {
        int i;
        float f;

        // load data of the spring
        dataSrc.readInteger(i);
        dataSrc.readInteger(i);
        dataSrc.readFloat(f);
        dataSrc.readFloat(f);
    }

    for (int morphId = 0; morphId < morphCount; morphId++) {
        std::string morphName;
        dataSrc.readString(morphName);

        int blendVertId;
        dataSrc.readInteger(blendVertId);

        CalCoreMorphTarget::VertexOffsetArray vertexOffsets;

        for (int blendVertI = 0; blendVertI < vertexCount; blendVertI++) {
            VertexOffset Vertex;

            if (blendVertI >= blendVertId) {
                CalVectorFromDataSrc(dataSrc, &Vertex.position);
                CalVectorFromDataSrc(dataSrc, &Vertex.normal);
                for (int textureCoordinateId = 0; textureCoordinateId < textureCoordinateCount; ++textureCoordinateId) {
                    CalCoreSubmesh::TextureCoordinate textureCoordinate;
                    dataSrc.readFloat(textureCoordinate.u);
                    dataSrc.readFloat(textureCoordinate.v);
                }

                Vertex.vertexId = blendVertI;
                Vertex.position -= pCoreSubmesh->getVectorVertex()[blendVertI].position;
                Vertex.normal -= pCoreSubmesh->getVectorVertex()[blendVertI].normal;
                vertexOffsets.push_back(Vertex);
                dataSrc.readInteger(blendVertId);
            }
        }

        if (isValidUTF8(morphName) == false) {
            morphName = convertLatin1toUTF8(morphName);
        }

        ISTD::shared_ptr<CalCoreMorphTarget> morphTarget = ISTD::make_shared<CalCoreMorphTarget>(morphName, vertexCount, vertexOffsets);
        pCoreSubmesh->addMorphTarget(morphTarget);
    }

    for (int faceId = 0; faceId < faceCount; ++faceId) {
        int tmp[3];
        dataSrc.readInteger(tmp[0]);
        dataSrc.readInteger(tmp[1]);
        dataSrc.readInteger(tmp[2]);

        if (tmp[0] > 65535 || tmp[1] > 65535 || tmp[2] > 65535) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
            return ISTD::shared_ptr<CalCoreSubmesh>();
        }

        pCoreSubmesh->addFace(CalCoreSubmesh::Face(tmp[0], tmp[1], tmp[2]));
    }

    return pCoreSubmesh;
}

ISTD::shared_ptr<CalCoreTrack> CalLoader::loadCoreTrack(
    CalBufferSource& dataSrc,
    int version,
    bool useAnimationCompression
) {
    const ISTD::shared_ptr<CalCoreTrack> null;

    // Read the bone id.
    int coreBoneId;
    bool translationRequired = true;
    bool highRangeRequired = true;
    bool translationIsDynamic = true;
    int keyframeCount;
    static unsigned char buf[ 4 ];

    // If this file version supports animation compression, then I store the boneId in 15 bits,
    // and use the 16th bit to record if translation is required.
    if (useAnimationCompression) {
        if (!dataSrc.readBytes(buf, 4)) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
            return null;
        }

        // Stored low byte first.  Top 3 bits of coreBoneId are compression flags.
        coreBoneId = buf[ 0 ] + (unsigned int)(buf[ 1 ] & 0x1f) * 256;
        translationRequired = (buf[ 1 ] & 0x80) ? true : false;
        highRangeRequired = (buf[ 1 ] & 0x40) ? true : false;
        translationIsDynamic = (buf[ 1 ] & 0x20) ? true : false;
        keyframeCount = buf[ 2 ] + (unsigned int) buf[ 3 ] * 256;
    } else {
        if (!dataSrc.readInteger(coreBoneId) || (coreBoneId < 0)) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
            return null;
        }

        // Read the number of keyframes.
        if (!dataSrc.readInteger(keyframeCount) || (keyframeCount <= 0)) {
            CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
            return null;
        }
    }

    CalCoreTrack::KeyframeList keyframes;

    keyframes.reserve(keyframeCount);

    // load all core keyframes
    bool hasLastKeyframe = false;
    CalCoreKeyframe lastCoreKeyframe;
    for (int keyframeId = 0; keyframeId < keyframeCount; ++keyframeId) {
        // load the core keyframe
        std::pair<bool, CalCoreKeyframe> coreKeyframe = loadCoreKeyframe(
            dataSrc,
            version,
            (hasLastKeyframe ? &lastCoreKeyframe : 0),
            translationRequired,
            highRangeRequired,
            translationIsDynamic,
            useAnimationCompression
        );

        if (coreKeyframe.first == false) {
            return null;
        }

        // add the core keyframe to the core track instance

        lastCoreKeyframe = coreKeyframe.second;
        hasLastKeyframe = true;
        keyframes.push_back(coreKeyframe.second);
    }

    ISTD::shared_ptr<CalCoreTrack> pCoreTrack = ISTD::make_shared<CalCoreTrack>(coreBoneId, keyframes);
    pCoreTrack->translationRequired = translationRequired;
    pCoreTrack->translationIsDynamic = translationIsDynamic;
    return pCoreTrack;
}


/*****************************************************************************/
/** Loads a core morphTrack instance.
 *
 * This function loads a core morphTrack instance from a data source.
 *
 * @param dataSrc The data source to load the core morphTrack instance from.
 *
 * @return One of the following values:
 *         \li a pointer to the core morphTrack
 *         \li \b 0 if an error happened
 *****************************************************************************/

ISTD::shared_ptr<CalCoreMorphTrack> CalLoader::loadCoreMorphTrack(CalBufferSource& dataSrc) {
    const ISTD::shared_ptr<CalCoreMorphTrack> null;
    
    // read the morph name
    std::string morphName;
    if (!dataSrc.readString(morphName)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    // allocate a new core morphTrack instance
    ISTD::shared_ptr<CalCoreMorphTrack> pCoreMorphTrack = ISTD::make_shared<CalCoreMorphTrack>();

    // link the core morphTrack to the appropriate morph name
    pCoreMorphTrack->morphName = morphName;

    // read the number of keyframes
    int keyframeCount;
    if (!dataSrc.readInteger(keyframeCount) || (keyframeCount < 0)) {
        CalError::setLastError(CalError::INVALID_FILE_FORMAT, __FILE__, __LINE__);
        return null;
    }

    pCoreMorphTrack->keyframes.reserve(keyframeCount);

    // load all core keyframes
    for (int keyframeId = 0; keyframeId < keyframeCount; ++keyframeId) {
        // load and add the core keyframe to the core morphTrack instance
        pCoreMorphTrack->keyframes.push_back(loadCoreMorphKeyframe(dataSrc));
    }

    return pCoreMorphTrack;
}
