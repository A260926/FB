//****************************************************************************//
// coreskeleton.cpp                                                           //
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

#include <queue>
#include <set>
#include <map>
#include <regex>
#include "cal3d/error.h"
#include "cal3d/coreskeleton.h"
#include "cal3d/corebone.h"

std::vector<ISTD::shared_ptr<CalCoreBone>> findInvalidParents(
    const std::vector<ISTD::shared_ptr<CalCoreBone>>& bones,
    const std::map<unsigned, ISTD::shared_ptr<CalCoreBone>>& originalIndices
) {
    std::set<ISTD::shared_ptr<CalCoreBone>> unknown(bones.begin(), bones.end());

    std::vector<ISTD::shared_ptr<CalCoreBone>> knownInvalid;
    while (!unknown.empty()) {
        ISTD::shared_ptr<CalCoreBone> p = *unknown.begin();

        std::set<ISTD::shared_ptr<CalCoreBone>> path;
        
        path.insert(p);
        while (p->parentId != -1) {
            if (size_t(p->parentId) >= bones.size()) {
                knownInvalid.insert(knownInvalid.end(), path.begin(), path.end());
                break;
            }

            ISTD::shared_ptr<CalCoreBone> next = cal3d::map_get(originalIndices, static_cast<unsigned>(p->parentId));
            if (path.count(next)) {
                knownInvalid.insert(knownInvalid.end(), path.begin(), path.end());
                break;
            }
            p = next;
            path.insert(p);
        }

        // clear path
        for (std::set<ISTD::shared_ptr<CalCoreBone>>::iterator i = path.begin(); i != path.end(); ++i) {
            unknown.erase(*i);
        }
    }
    return knownInvalid;
}

std::regex isSeatNode("(Seat|Url|Pitcher|Catcher|Handle)", std::regex::icase);
std::regex processSeatNode("(.*?)(\\d*)(\\..*)?$", std::regex::icase);
std::regex isFurniNode("furniture\\.(Floor|Wall|Ceiling)[^.]*\\.(([0-9][0-9]?)|([1-9][0-9]+))$", std::regex::icase);
std::regex processFurniNode("furniture\\.(Floor|Wall|Ceiling)[^.]*\\.(\\d+)$", std::regex::icase);
std::regex isCameraNode("camera\\.\\d\\d(-Stop)?\\.\\d\\d(-Stop)?(\\.root)?", std::regex::icase);
std::regex processCameraNode("camera.\\d+(.*)");

CalCoreSkeletonPtr CalCoreSkeleton::deduplicateSkeleton() {
    auto bones = m_coreBones;
    CalCoreSkeletonPtr newSkeleton(new CalCoreSkeleton());
    std::set<std::string> boneNames;
    std::map<std::string, unsigned> boneDedups;
    for (size_t i = 0; i < bones.size(); ++i) {
        if (boneNames.find(bones[i]->name) == boneNames.end()) {
            boneNames.insert(bones[i]->name);
            newSkeleton->addCoreBone(bones[i]);
        } else {
            std::string namePrefix = "";
            std::string nameSuffix = "";
            int bigNum = 10000;
            std::string dedupName = "";
            std::smatch matchResults;
            if (regex_search(bones[i]->name, isSeatNode)) {
                regex_match(bones[i]->name, matchResults, processSeatNode);
                namePrefix = matchResults[1];
                nameSuffix = matchResults[3];
                dedupName = namePrefix+nameSuffix;
            } else if (regex_search(bones[i]->name, isFurniNode)) {
                regex_match(bones[i]->name, matchResults, processFurniNode);
                namePrefix = std::string("furniture.") + matchResults[1].str() + std::string(".");
            } else if (regex_search(bones[i]->name, isCameraNode)) {
                regex_match(bones[i]->name, matchResults, processCameraNode);
                namePrefix = "camera.";
                nameSuffix = matchResults[1];
                bigNum = 100;
            } else {
                namePrefix = bones[i]->name + "-";
            }
            long long dedupNum = bigNum;
            auto it = boneDedups.find(namePrefix);
            if (boneDedups.end() == it) {
                boneDedups.insert(std::pair<std::string, unsigned>(namePrefix, bigNum-1)); // This is deliberately wrong.
                dedupNum = bigNum-1; // this forces a dupe, because that's what N* is doing - we should see two copies of 9999. yes this is dumb.
            } else {
                dedupNum = it->second--;
            }
            std::string newName = (namePrefix + std::to_string(dedupNum) + nameSuffix);
            CalCoreBonePtr newBone(new CalCoreBone(newName, bones[i]->parentId));
            newBone->lightColor = bones[i]->lightColor;
            newBone->lightType = bones[i]->lightType;
            newBone->relativeTransform = bones[i]->relativeTransform;
            newBone->inverseBindPoseTransform = bones[i]->inverseBindPoseTransform;
            newSkeleton->addCoreBone(newBone);
        }

        newSkeleton->setBoneIdTranslation(i, getBoneIdTranslation(i));
    }
    return newSkeleton;
}


CalCoreSkeleton::CalCoreSkeleton(const std::vector<ISTD::shared_ptr<CalCoreBone>>& bones)
    : coreBones(m_coreBones)
{
    m_coreBones.reserve(bones.size());

    std::map<unsigned, ISTD::shared_ptr<CalCoreBone>> originalIndices;
    for (size_t i = 0; i < bones.size(); ++i) {
        originalIndices[i] = bones[i];
    }
    std::vector<ISTD::shared_ptr<CalCoreBone>> invalidParents = findInvalidParents(bones, originalIndices);
    for (size_t i = 0; i < invalidParents.size(); ++i) {
        invalidParents[i]->parentId = -1;
    }

    std::queue<ISTD::shared_ptr<CalCoreBone>> q;
    for (size_t i = 0; i < bones.size(); ++i) {
        q.push(bones[i]);
    }

    std::set<ISTD::shared_ptr<CalCoreBone>> added;

    std::map<ISTD::shared_ptr<CalCoreBone>, unsigned> newIndices;

    while (!q.empty()) {
        ISTD::shared_ptr<CalCoreBone> p = q.front();
        q.pop();

        if (p->parentId == -1 || added.count(originalIndices[p->parentId])) {
            if (p->parentId != -1) {
                p->parentId = newIndices[originalIndices[p->parentId]];
            }
            newIndices[p] = addCoreBone(p);
            added.insert(p);
        } else {
            q.push(p);
        }
    }

    boneIdTranslation.resize(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        boneIdTranslation[i] = newIndices[originalIndices[i]];
    }

    cal3d::verify(added.size() == bones.size(), "should have added all bones");
    cal3d::verify(newIndices.size() == originalIndices.size(), "new and original index lists should have identical lengths");
    cal3d::verify(boneIdTranslation.size() == m_coreBones.size(), "ID->index translation table and bone list must have identical lengths");
}

size_t CalCoreSkeleton::addCoreBone(const ISTD::shared_ptr<CalCoreBone>& coreBone) {
    cal3d::verify(coreBone->parentId == -1 || size_t(coreBone->parentId) < coreBones.size(), "bones must be added in topological order");

    // skeletons can only have one root
    if (coreBone->parentId == -1) {
        if (m_coreBones.empty()) {
            inverseOriginalRootTransform = invert(coreBone->relativeTransform);
            coreBone->relativeTransform = cal3d::RotateTranslate();
        } else {
            coreBone->parentId = 0;
            coreBone->relativeTransform = inverseOriginalRootTransform * coreBone->relativeTransform;
        }
        adjustedRoots.insert(m_coreBones.size());
    }

    size_t newIndex = m_coreBones.size();
    m_coreBones.push_back(coreBone);

    boneIdTranslation.push_back(newIndex);
    return newIndex;
}

void CalCoreSkeleton::setCoreBone(size_t index, const ISTD::shared_ptr<CalCoreBone>& coreBone) {
    m_coreBones[index] = coreBone;
}

int CalCoreSkeleton::getBoneId(const CalCoreBone* coreBone) const {
    for (size_t i = 0; i < coreBones.size(); ++i) {
        if (coreBones[i].get() == coreBone) {
            return i;
        }
    }
    return -1;
}

void CalCoreSkeleton::scale(float factor) {
    for (auto i = coreBones.begin(); i != coreBones.end(); ++i) {
        (*i)->scale(factor);
    }
}

cal3d::RotateTranslate CalCoreSkeleton::getAdjustedRootTransform(size_t boneIndex) const {
    if (adjustedRoots.count(boneIndex)) {
        return inverseOriginalRootTransform;
    } else {
        return cal3d::RotateTranslate();
    }
}

std::vector<int> CalCoreSkeleton::getChildIds(const CalCoreBone* coreBone) const {
    int index = -1;
    if (coreBone) {
        for (size_t i = 0; i < coreBones.size(); ++i) {
            if (coreBones[i].get() == coreBone) {
                index = i;
                break;
            }
        }
        cal3d::verify(index != -1, "Cannot calculate children of bone not owned by this skeleton");
    } else {
        // find roots
    }

    std::vector<int> rv;
    for (size_t i = 0; i < coreBones.size(); ++i) {
        if (coreBones[i]->parentId == index) {
            rv.push_back(i);
        }
    }
    return rv;
}

 void CalCoreSkeleton::rotateTranslate(cal3d::RotateTranslate& rt) {
    for (size_t i = 0; i < m_coreBones.size(); ++i) {
        if (m_coreBones[i]->parentId == -1) {
            m_coreBones[i]->rotateTranslate(rt);
        }
    }
    inverseOriginalRootTransform = inverseOriginalRootTransform * rt;
 }

void CalCoreSkeleton::rotate(CalQuaternion &rot) {
    cal3d::RotateTranslate rt(rot, CalVector(0, 0, 0));
    rotateTranslate(rt);
}

unsigned CalCoreSkeleton::getBoneIdTranslation(unsigned index) {
    if (index < boneIdTranslation.size()) {
        return boneIdTranslation[index];
    }

    return static_cast<signed>(-1);
}

void CalCoreSkeleton::setBoneIdTranslation(unsigned originalIndex, unsigned newIndex) {
    if (originalIndex < boneIdTranslation.size()) {
        boneIdTranslation[originalIndex] = newIndex;
    }
}

