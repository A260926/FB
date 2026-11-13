//****************************************************************************//
// coremesh.cpp                                                               //
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

#include <stdio.h>
#include "cal3d/coremesh.h"
#include "cal3d/error.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coremorphtarget.h"


void CalCoreMesh()
{
    CalError::setLastError(CalError::INTERNAL, __FILE__, __LINE__, "In CalCoreMesh constructor");
}

size_t sizeInBytes(const ISTD::shared_ptr<CalCoreSubmesh>& submesh) {
    return sizeof(ISTD::shared_ptr<CalCoreSubmesh>) + submesh->sizeInBytes();
}

size_t CalCoreMesh::sizeInBytes() const {
    return sizeof(*this) + ::sizeInBytes(submeshes);
}

bool CalCoreMesh::addAsMorphTarget(CalCoreMesh* pCoreMesh, std::string const& morphTargetName) {
    //Check if the numbers of vertices allow a blending
    CalCoreMesh::CalCoreSubmeshVector& otherVectorCoreSubmesh = pCoreMesh->submeshes;
    if(submeshes.size() == 0) {
        submeshes = otherVectorCoreSubmesh;
    }
    if (submeshes.size() != otherVectorCoreSubmesh.size()) {
            char buf[2048] = {0}; // zero-initialize
#ifdef _MSC_VER
            _snprintf
#else
            snprintf
#endif
            (buf, sizeof(buf) - 1, "Mesh %s has a morph target with differing number of materials: %zu vs %zu", morphTargetName.c_str(), submeshes.size(), otherVectorCoreSubmesh.size());
            CalError::setLastError(CalError::INTERNAL, __FILE__, __LINE__, buf);
        //CalError::setLastError(CalError::INTERNAL, __FILE__, __LINE__, "This mesh has children with different numbers of materials");
        return false;
    }
    if (submeshes.size() == 0) {
        CalError::setLastError(CalError::INTERNAL, __FILE__, __LINE__, "Mesh has no submeshes");
        return false;
    }
    CalCoreMesh::CalCoreSubmeshVector::iterator iteratorCoreSubmesh = submeshes.begin();
    CalCoreMesh::CalCoreSubmeshVector::iterator otherIteratorCoreSubmesh = otherVectorCoreSubmesh.begin();
    std::vector<int> submeshValid;
    unsigned int submeshIndex = 0;
    while (iteratorCoreSubmesh != submeshes.end()) {
        size_t count1 = (*iteratorCoreSubmesh)->getVertexCount();
        size_t count2 = (*otherIteratorCoreSubmesh)->getVertexCount();

        if (count1 != count2) {
            char buf[2048] = {0}; // zero-initialize
#ifdef _MSC_VER
            _snprintf
#else
            snprintf
#endif
            (buf, sizeof(buf) - 1, "Mesh %s has a morph target child (%d) with different number of vertices: %zu vs child's %zu", morphTargetName.c_str(), submeshIndex+1, count1, count2);
            CalError::setLastError(CalError::INTERNAL, __FILE__, __LINE__, buf);
            return false; 
        }
        ISTD::shared_ptr<CalCoreSubmesh> baseMesh = (*iteratorCoreSubmesh);
        ISTD::shared_ptr<CalCoreSubmesh> morphMesh = (*otherIteratorCoreSubmesh);
        count1 = baseMesh->getFaceCount();
        count2 = morphMesh->getFaceCount();
        if (count1 != count2) {
            char buf[2048] = {0}; // zero-initialize
#ifdef _MSC_VER
            _snprintf
#else
            snprintf
#endif
            (buf, sizeof(buf) - 1, "Mesh %s has a morph target child (%d) with different number of faces: %zu vs child's %zu", morphTargetName.c_str(), submeshIndex+1, count1, count2);
            CalError::setLastError(CalError::INTERNAL, __FILE__, __LINE__, buf);
            return false; 
        }
        CalCoreSubmesh::VectorFace baseFaces = baseMesh->getFaces();
        CalCoreSubmesh::VectorFace morphFaces = morphMesh->getFaces();
        unsigned int i = 0;
        for(i = 0; i < count1; i++)
        {
            CalCoreSubmesh::Face f1 = baseFaces[i];
            CalCoreSubmesh::Face f2 = morphFaces[i];
            if(f1 == f2)
                continue;
            else
            {
                char buf[2048] = {0}; // zero-initialize
#ifdef _MSC_VER
             _snprintf
#else
             snprintf
#endif
                (buf, sizeof(buf) - 1, "Mesh %s has a morph target child (%d) with inconsistent topology", morphTargetName.c_str(), submeshIndex + 1);
                CalError::setLastError(CalError::INTERNAL, __FILE__, __LINE__, buf);
                //break; 
                return false; 
            }
        }
        if(i == count1)
            submeshValid.push_back(1);
        else
            submeshValid.push_back(0);

        ++submeshIndex;
        ++iteratorCoreSubmesh;
        ++otherIteratorCoreSubmesh;
    }
    //Adding the blend targets to each of the core sub meshes
    iteratorCoreSubmesh = submeshes.begin();
    otherIteratorCoreSubmesh = otherVectorCoreSubmesh.begin();
    submeshIndex = 0;
    while (iteratorCoreSubmesh != submeshes.end()) {
        if(submeshValid[submeshIndex] == 1)
        {
            size_t vertexCount = (*otherIteratorCoreSubmesh)->getVertexCount();
            const CalCoreSubmesh::VectorVertex& vectorCoreVertex = (*iteratorCoreSubmesh)->getVectorVertex();
            const CalCoreSubmesh::VectorVertex& vectorVertex = (*otherIteratorCoreSubmesh)->getVectorVertex();

            CalCoreMorphTarget::VertexOffsetArray vertices;
            for (unsigned i = 0; i < vertexCount; ++i) {
                vertices.push_back(VertexOffset(
                    i,
                    vectorVertex[i].position - vectorCoreVertex[i].position,
                    vectorVertex[i].normal - vectorCoreVertex[i].normal));
            }
            ISTD::shared_ptr<CalCoreMorphTarget> pCalCoreMorphTarget = ISTD::make_shared<CalCoreMorphTarget>(morphTargetName, vertexCount, vertices);
            (*iteratorCoreSubmesh)->addMorphTarget(pCalCoreMorphTarget);
        }
        ++submeshIndex;
        ++iteratorCoreSubmesh;
        ++otherIteratorCoreSubmesh;
    }
    return true;
}

void CalCoreMesh::replaceMeshWithMorphTarget(const std::string& morphTargetName) {
    for (auto i = submeshes.begin(); i != submeshes.end(); ++i) {
        (*i)->replaceMeshWithMorphTarget(morphTargetName);
    }
}

void CalCoreMesh::scale(float factor) {
    for (auto i = submeshes.begin(); i != submeshes.end(); ++i) {
        (*i)->scale(factor);
    }
}

void CalCoreMesh::fixup(const ISTD::shared_ptr<CalCoreSkeleton>& skeleton) {
    for (auto i = submeshes.begin(); i != submeshes.end(); ++i) {
        (*i)->fixup(skeleton);
    }
}

void CalCoreMesh::removeEmptySubmeshes() {
    CalCoreSubmeshVector ns;
    for (auto i = submeshes.begin(); i != submeshes.end(); ++i) {
        if ((*i)->getFaceCount()) {
            ns.push_back(*i);
        }
    }
    std::swap(ns, submeshes);
}

void CalCoreMesh::removeMorpTargetsWithEmptyNames() {
    for (auto i = submeshes.begin(); i != submeshes.end(); ++i) {
        (*i)->removeMorpTargetsWithEmptyNames();
    }
}

void CalCoreMesh::splitSubmeshesToBoneLimit(unsigned boneLimit) {
    // If every vertex references 4 unique bones then 12 is the limit
    // and we will end up with one triangle per submesh.
    //assert(boneLimit >= 12);
    //
    // however...  the algorithm won't fail if a lower limit is
    // passed: it will just produce some meshes with 12 bones per
    // triangle.

    CalCoreSubmeshVector out;
    for (auto i = submeshes.begin(); i != submeshes.end(); ++i) {
        (*i)->splitSubmeshesToBoneLimit(out, boneLimit);
    }
    std::swap(out, submeshes);
}
