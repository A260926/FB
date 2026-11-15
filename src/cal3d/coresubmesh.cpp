//****************************************************************************//
// coresubmesh.cpp                                                            //
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

#include "cal3d/coreskeleton.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/coremorphtarget.h"
#include "cal3d/vector4.h"
#include "cal3d/transform.h"
#include "cal3d/forsythtriangleorderoptimizer.h"
#include "cal3d/trisort.h"
#include "cal3d/simplify.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>

const int CalCoreSubmesh::m_maxInfluencesPerVertex = 10;

CalCoreSubmesh::CalCoreSubmesh(int vertexCount, bool hasTextureCoordinates, int faceCount)
    : coreMaterialThreadId(0)
    , m_currentVertexId(0)
    , m_vertices(vertexCount)
    , m_isStatic(false)
    , m_minimumVertexBufferSize(0)
{
    m_vertexColors.resize(vertexCount);

    if (hasTextureCoordinates) {
        m_textureCoordinates.resize(vertexCount);
    }

    m_faces.reserve(faceCount);
}

CAL3D_DEFINE_SIZE(CalCoreSubmesh::Face);
CAL3D_DEFINE_SIZE(CalCoreSubmesh::Influence);

size_t sizeInBytes(const CalCoreSubmesh::InfluenceSet& is) {
    return sizeof(is) + sizeInBytes(is.influences);
}

size_t CalCoreSubmesh::sizeInBytes() const {
    size_t r = sizeof(*this);
    r += ::sizeInBytes(m_vertices);
    r += ::sizeInBytes(m_vertexColors);
    r += ::sizeInBytes(m_faces);
    r += ::sizeInBytes(m_staticInfluenceSet);
    r += ::sizeInBytes(m_influences);
    return r;
}

void CalCoreSubmesh::addFace(const CalCoreSubmesh::Face& face) {
    for (int i = 0; i < 3; ++i) {
        m_minimumVertexBufferSize = std::max(m_minimumVertexBufferSize, size_t(1 + face.vertexId[i]));
    }
    m_faces.push_back(face);
}

void CalCoreSubmesh::setTextureCoordinate(int vertexId, const TextureCoordinate& textureCoordinate) {
    m_textureCoordinates[vertexId] = textureCoordinate;
}

bool CalCoreSubmesh::validateSubmesh() const {
    size_t vertexCount = m_vertices.size(); 
    size_t numFaces = m_faces.size();
    for (size_t f = 0; f < numFaces; ++f) {
        if (m_faces[f].vertexId[0] >= vertexCount || m_faces[f].vertexId[1] >= vertexCount || m_faces[f].vertexId[2] >= vertexCount) {
            return false;
        }
    }
    return true;
}

bool CalCoreSubmesh::hasTextureCoordinatesOutside0_1() const {
    for (auto i = m_textureCoordinates.begin(); i != m_textureCoordinates.end(); ++i) {
        if (
            i->u < 0 || i->u > 1 ||
            i->v < 0 || i->v > 1
        ) {
            return true;
        }
    }
    return false;
}

static bool descendingByWeight(const CalCoreSubmesh::Influence& lhs, const CalCoreSubmesh::Influence& rhs) {
    return lhs.weight > rhs.weight;
}

void CalCoreSubmesh::addVertex(const Vertex& vertex, CalColor32 vertexColor, const std::vector<Influence>& inf_) {
    assert(m_currentVertexId < m_vertices.size());

    const int vertexId = m_currentVertexId++;
    if (vertexId == 0) {
        m_isStatic = true;
        m_staticInfluenceSet = inf_;
        m_boundingVolume.min = vertex.position.asCalVector();
        m_boundingVolume.max = vertex.position.asCalVector();
    } else if (m_isStatic) {
        m_isStatic = m_staticInfluenceSet == inf_;
    }

    if (vertexId) {
        m_boundingVolume.min.x = std::min(m_boundingVolume.min.x, vertex.position.x);
        m_boundingVolume.min.y = std::min(m_boundingVolume.min.y, vertex.position.y);
        m_boundingVolume.min.z = std::min(m_boundingVolume.min.z, vertex.position.z);

        m_boundingVolume.max.x = std::max(m_boundingVolume.max.x, vertex.position.x);
        m_boundingVolume.max.y = std::max(m_boundingVolume.max.y, vertex.position.y);
        m_boundingVolume.max.z = std::max(m_boundingVolume.max.z, vertex.position.z);
    }

    m_vertices[vertexId] = vertex;
    m_vertexColors[vertexId] = vertexColor;

    // Each vertex needs at least one influence.
    std::vector<Influence> inf(inf_);
    if (inf.empty()) {
        m_isStatic = false;
        Influence i;
        i.boneId = 0;
        i.weight = 0.0f;
        inf.push_back(i);
    } else {
        std::sort(inf.begin(), inf.end(), descendingByWeight);
    }

    // Mark the last influence as the last one.  :)
    for (size_t i = 0; i + 1 < inf.size(); ++i) {
        inf[i].lastInfluenceForThisVertex = 0;
    }
    inf[inf.size() - 1].lastInfluenceForThisVertex = 1;

    m_influences.insert(m_influences.end(), inf.begin(), inf.end());
}

void CalCoreSubmesh::scale(float factor) {
    // needed because we shouldn't modify the w term
    CalVector4 scaleFactor(factor, factor, factor, 1.0f);

    for (size_t vertexId = 0; vertexId < m_vertices.size(); vertexId++) {
        m_vertices[vertexId].position *= scaleFactor;
    }

    m_boundingVolume.min *= factor;
    m_boundingVolume.max *= factor;

    for (MorphTargetArray::iterator i = m_morphTargets.begin(); i != m_morphTargets.end(); ++i) {
        (*i)->scale(factor);
    }
}


void CalCoreSubmesh::fixup(const ISTD::shared_ptr<CalCoreSkeleton>& skeleton) {
    for (size_t i = 0; i < m_influences.size(); ++i) {
        Influence& inf = m_influences[i];
        inf.boneId = (inf.boneId < skeleton->boneIdTranslation.size())
            ? skeleton->boneIdTranslation[inf.boneId]
            : 0;
    }

    std::set<Influence> staticInfluenceSet;

    for (std::set<Influence>::iterator i = m_staticInfluenceSet.influences.begin(); i != m_staticInfluenceSet.influences.end(); ++i) {
        Influence inf = *i;
        inf.boneId = (inf.boneId < skeleton->boneIdTranslation.size())
            ? skeleton->boneIdTranslation[inf.boneId]
            : 0;
        staticInfluenceSet.insert(inf);
    }

    std::swap(m_staticInfluenceSet.influences, staticInfluenceSet);
}

bool CalCoreSubmesh::isStatic() const {
    return m_isStatic && m_morphTargets.empty();
}

BoneTransform CalCoreSubmesh::getStaticTransform(const BoneTransform* bones) const {
    BoneTransform rm;

    std::set<Influence>::const_iterator current = m_staticInfluenceSet.influences.begin();
    while (current != m_staticInfluenceSet.influences.end()) {
        const BoneTransform& influence = bones[current->boneId];
        rm.rowx += current->weight * influence.rowx;
        rm.rowy += current->weight * influence.rowy;
        rm.rowz += current->weight * influence.rowz;

        ++current;
    }

    return rm;
}

void CalCoreSubmesh::addMorphTarget(const ISTD::shared_ptr<CalCoreMorphTarget>& morphTarget) {
    if (morphTarget->vertexOffsets.size() > 0) {
        m_morphTargets.push_back(morphTarget);
    }
}

void CalCoreSubmesh::replaceMeshWithMorphTarget(const std::string& morphTargetName) {
    for (auto i = m_morphTargets.begin(); i != m_morphTargets.end(); ++i) {
        if ((*i)->name == morphTargetName) {
            const auto& offsets = (*i)->vertexOffsets;
            for (auto o = offsets.begin(); o != offsets.end(); ++o) {
                m_vertices[o->vertexId].position += o->position;
                m_vertices[o->vertexId].normal += o->normal;
            }
        }
    }
}

/*
          f8, f9
            :
  (0,1,0) __:_____(1,1,0)
         /  :    /|
 (0,1,1)/_______/-|------ f2, f3
        |       | |(1,0, 0)         y
        | f0,f1 | /                 |__ x
        |_______|/                 /
  (0,0,1)   :  (1,0,1)            z
            :
         f10, f11


*/

 
ISTD::shared_ptr<CalCoreSubmesh> MakeCubeScale(float scale) {
    ISTD::shared_ptr<CalCoreSubmesh> cube = ISTD::make_shared<CalCoreSubmesh>(24, true, 12);
    
    const CalColor32 black = 0;
    
    std::vector<CalCoreSubmesh::Influence> inf(1);
    inf[0].boneId = 0;
    inf[0].weight = 1.0f;
    
    int curVertexId=0;
    CalCoreSubmesh::TextureCoordinate texCoord;
    CalCoreSubmesh::Vertex vertex;
    //triangle face f0, f1 vertices
    //v0
    curVertexId = 0;
    vertex.position = CalPoint4(0, 0, scale);
    vertex.normal = CalVector4(0, 0, 1);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 0.0f;
    cube->setTextureCoordinate(curVertexId, texCoord);
    //v1
    ++curVertexId;
    vertex.position = CalPoint4(scale,scale,scale);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 1.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v2
    ++curVertexId;
    vertex.position = CalPoint4(0,scale,scale);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v3
    ++curVertexId;
    vertex.position = CalPoint4(scale,0,scale);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 1.0f;
    texCoord.v = 0.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    
    cube->addFace(CalCoreSubmesh::Face(0, 1, 2));
    cube->addFace(CalCoreSubmesh::Face(0, 3, 1));
    
    //triangle faces f2, f3 vertices
    //v4
    
    ++curVertexId;
    vertex.position = CalPoint4(scale, 0, scale);
    vertex.normal = CalVector4(1, 0, 0);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 0.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v5
    ++curVertexId;
    vertex.position = CalPoint4(scale,scale,0);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 1.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v6
    ++curVertexId;
    vertex.position = CalPoint4(scale,scale,scale);
    cube->addVertex( vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v7
    ++curVertexId;
    vertex.position = CalPoint4(scale,0,0);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 1.0f;
    texCoord.v = 0.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    
    cube->addFace(CalCoreSubmesh::Face(4, 5, 6));
    cube->addFace(CalCoreSubmesh::Face(4, 7, 5));
    
    //triangle faces f4, f5 vertices
    //v8
    ++curVertexId;
    vertex.position = CalPoint4(scale, 0, 0);
    vertex.normal = CalVector4(0, 0, -1);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 0.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v9
    ++curVertexId;
    vertex.position = CalPoint4(0,scale,0);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 1.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v10
    ++curVertexId;
    vertex.position = CalPoint4(scale,scale,0);
    cube->addVertex( vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v11
    ++curVertexId;
    vertex.position = CalPoint4(0,0,0);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 1.0f;
    texCoord.v = 0.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    
    CalCoreSubmesh::Face f4(8, 9, 10);
    cube->addFace(f4);
    CalCoreSubmesh::Face f5(8, 11, 9);
    cube->addFace(f5);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);

    //triangle faces f6, f7 vertices
    //v12
    ++curVertexId;
    vertex.position = CalPoint4(0, 0, 0);
    vertex.normal = CalVector4(-1, 0, 0);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 0.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v13
    ++curVertexId;
    vertex.position = CalPoint4(0,scale,scale);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 1.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v14
    ++curVertexId;
    vertex.position = CalPoint4(0,scale,0);
    cube->addVertex( vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v15
    ++curVertexId;
    vertex.position = CalPoint4(0,0,scale);
    cube->addVertex(vertex, black,inf);
    texCoord.u = 1.0f;
    texCoord.v = 0.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    
    CalCoreSubmesh::Face f6(12, 13, 14);
    cube->addFace(f6);
    CalCoreSubmesh::Face f7(12, 15, 13);
    cube->addFace(f7);
    
    //triangle faces f8, f9 vertices
    //v16
    ++curVertexId;
    vertex.position = CalPoint4(0, scale, scale);
    vertex.normal = CalVector4(0, 1, 0);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v17
    ++curVertexId;
    vertex.position = CalPoint4(scale,scale,0);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v18
    ++curVertexId;
    vertex.position = CalPoint4(0,scale,0);
    cube->addVertex( vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v19
    ++curVertexId;
    vertex.position = CalPoint4(scale,scale,scale);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    
    CalCoreSubmesh::Face f8(16, 17, 18);
    cube->addFace(f8);
    CalCoreSubmesh::Face f9(16, 19, 17);
    cube->addFace(f9);
    
    //triangle faces f10, f11 vertices
    //v20
    ++curVertexId;
    vertex.position = CalPoint4(scale, 0, 0);
    vertex.normal = CalVector4(0, -1, 0);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v21
    ++curVertexId;
    vertex.position = CalPoint4(0,0,scale);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v22
    ++curVertexId;
    vertex.position = CalPoint4(0,0,0);
    cube->addVertex( vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId,  texCoord);
    //v23
    ++curVertexId;
    vertex.position = CalPoint4(scale,0,scale);
    cube->addVertex(vertex, black, inf);
    texCoord.u = 0.0f;
    texCoord.v = 1.0f;
    cube->setTextureCoordinate(curVertexId, texCoord);
    
    CalCoreSubmesh::Face f10(20, 21, 22);
    cube->addFace(f10);
    CalCoreSubmesh::Face f11(20, 23, 21);
    cube->addFace(f11);
    return cube;
}

ISTD::shared_ptr<CalCoreSubmesh> MakeCube() {
    return MakeCubeScale(1.0f);
}

void CalCoreSubmesh::duplicateTriangles() {
    size_t faceCount = m_faces.size();

    std::vector<Face> newFaces;
    newFaces.reserve(2 * faceCount);

    for (size_t i = 0; i < faceCount; ++i) {
        const auto& face = m_faces[i];
        newFaces.push_back(face);
        newFaces.push_back(Face(face.vertexId[0], face.vertexId[2], face.vertexId[1]));
    }

    std::swap(m_faces, newFaces);
}

struct FaceSortRef {
    float fscore;
    unsigned face;
};

struct FaceSort {
    unsigned short nfront;      //  how many faces I am in front of
    size_t infront;       //  refs to faces I am in front of, with scores
    CalVector fnorm;            //  the face normal
    float fdist;                //  the face distance from origin in the direction of the normal
    float score;                //  the current cost of this face (sum of other faces behind)
    float area;
};

// http://www.mindcontrol.org/~hplus/graphics/sort-alpha.html
void CalCoreSubmesh::sortBackToFrontJonWatte() {
    size_t numFaces = m_faces.size();

    std::vector<FaceSort> facesStorage(numFaces);
    FaceSort* faces = cal3d::pointerFromVector(facesStorage);
    memset(faces, 0, sizeof(FaceSort) * numFaces);

    std::vector<FaceSortRef> frefs;

    for (size_t f = 0; f < numFaces; ++f) {
        faces[f].nfront = 0;
        faces[f].infront = 0;

        CalVector a = m_vertices[m_faces[f].vertexId[0]].position.asCalVector();
        CalVector b = m_vertices[m_faces[f].vertexId[1]].position.asCalVector();
        CalVector c = m_vertices[m_faces[f].vertexId[2]].position.asCalVector();
        b = b - a;
        c = c - a;
        a = cross(b, c);

        faces[f].area = a.length() * 0.5f;
        a.normalize();
        faces[f].fnorm = a;

        a = m_vertices[m_faces[f].vertexId[0]].position.asCalVector();
        faces[f].fdist = dot(a, faces[f].fnorm);
        //  delay score calculation
        faces[f].score = 0;
    }

    unsigned totalSearch = 0;
    for (size_t g = 0; g < numFaces; ++g) {
        assert(m_faces[g].vertexId[0] != m_faces[g].vertexId[1]);
        faces[g].infront = frefs.size();

        for (size_t f = 0; f < numFaces; ++f) {
            if (f == g) {
              continue;
            }

            int vfront = 0, gfront = 0;
            for (int i = 0; i < 3; ++i) {
                CalVector v = m_vertices[m_faces[g].vertexId[i]].position.asCalVector();
                float d = dot(v, faces[f].fnorm) - faces[f].fdist;
                if (d < -1e-4) {
                  vfront++;
                }

                v = m_vertices[m_faces[f].vertexId[i]].position.asCalVector();
                d = dot(v, faces[g].fnorm) - faces[g].fdist;
                if (d < 1e-4) {
                  gfront++;
                }

                totalSearch++;
            }

            if (vfront && (gfront < 3)) {
              FaceSortRef fr;
              fr.face = f;
              fr.fscore = (float)(std::max(faces[f].area, faces[g].area) * (1 + dot(faces[f].fnorm, faces[g].fnorm)) * 0.5 * vfront / 3);
              faces[g].nfront++;
              faces[f].score += fr.fscore;
              frefs.push_back(fr);
            }
        }
    }

    std::vector<Face> newFaces;

    Face newFace;
    for (size_t i = 0; i < numFaces; ++i) {
        float lowest = faces[i].score;
        size_t lowestIx = i;
        for (size_t f = 0; f < numFaces; ++f) {
            if (faces[f].score < lowest) {
                lowest = faces[f].score;
                lowestIx = f;
            }
        }
        assert(lowest < 1e20f);
        assert(m_faces[lowestIx].vertexId[0] != m_faces[lowestIx].vertexId[1]);
        assert(faces[lowestIx].score == lowest);

        newFace = m_faces[lowestIx];
        newFaces.push_back(newFace);

        faces[lowestIx].score = 1e20f;
        for (size_t q = 0; q < faces[lowestIx].nfront; ++q) {
          faces[frefs[faces[lowestIx].infront + q].face].score -= frefs[faces[lowestIx].infront + q].fscore;
        }
    }

    newFaces.swap(m_faces);
}

typedef std::vector<std::vector<CalCoreSubmesh::Influence>> InfluenceVectorVector;

static InfluenceVectorVector extractInfluenceVector(const CalCoreSubmesh::InfluenceVector& iv) {
    InfluenceVectorVector result;
    std::vector<CalCoreSubmesh::Influence> current;
    for (size_t i = 0; i < iv.size(); ++i) {
        auto& inf = iv[i];
        current.push_back(inf);
        if (inf.lastInfluenceForThisVertex) {
            result.push_back(current);
            current.clear();
        }
    }
    return result;
}

static CalCoreSubmesh::InfluenceVector generateInfluenceVector(const InfluenceVectorVector& iv) {
    CalCoreSubmesh::InfluenceVector result;
    for (auto influences = iv.begin(); influences != iv.end(); ++influences) {
        for (size_t i = 0; i + 1 < influences->size(); ++i) {
            auto j = (*influences)[i];
            j.lastInfluenceForThisVertex = false;
            result.push_back(j);
        }
        auto j = influences->back();
        j.lastInfluenceForThisVertex = true;
        result.push_back(j);
    }
    return result;
}

static ISTD::shared_ptr<CalCoreSubmesh> newEmptySubmeshWithVertices(const CalCoreSubmesh& self) {
    auto influenceVector = extractInfluenceVector(self.getInfluences());
    assert(influenceVector.size() == self.getVectorVertex().size());

    ISTD::shared_ptr<CalCoreSubmesh> newSubmesh(
        new CalCoreSubmesh(
            self.getVertexCount(),
            !self.getTextureCoordinates().empty(),
            self.getFaceCount() /* just a hint */));
    newSubmesh->coreMaterialThreadId = self.coreMaterialThreadId;

    for (size_t i = 0; i < self.getMorphTargets().size(); ++i) {
        newSubmesh->addMorphTarget(self.getMorphTargets()[i]);
    }

    for (size_t i = 0; i < self.getVertexCount(); ++i) {
        newSubmesh->addVertex(
            self.getVectorVertex()[i],
            self.getVertexColors()[i],
            influenceVector[i]);
        if (!self.getTextureCoordinates().empty()) {
            newSubmesh->setTextureCoordinate(i, self.getTextureCoordinates()[i]);
        }
    }

    return newSubmesh;
}

void CalCoreSubmesh::splitSubmeshesToBoneLimit(
    std::vector<ISTD::shared_ptr<CalCoreSubmesh>>& out,
    unsigned boneLimit
) const {
    if (m_faces.empty()) {
        return;
    }

    auto influenceVector = extractInfluenceVector(getInfluences());
    for (std::size_t i = 0; i < influenceVector.size(); i++) {
        if (influenceVector[i].size() > m_maxInfluencesPerVertex) {
            std::sort(influenceVector[i].begin(), influenceVector[i].end(),
                      [](const Influence& inf0, const Influence& inf1) { return inf0.weight > inf1.weight; });
            influenceVector[i].resize(m_maxInfluencesPerVertex);
        }
    }
    // This function produces meshes with lots of unused data.  That's
    // OK, subsequent passes like optimizeVertxCache and
    // renumberIndices will eliminate all of that.

    auto newSubmesh = newEmptySubmeshWithVertices(*this);

    std::set<unsigned> referencedBones;

    for (size_t i = 0; i < m_faces.size(); ++i) {
        const auto& face = m_faces[i];

        std::set<unsigned> thisTriangleBones;
        for (int t = 0; t < 3; ++t) {
            const auto& infs = influenceVector[face.vertexId[t]];
            for (size_t k = 0; k < infs.size(); ++k) {
                thisTriangleBones.insert(infs[k].boneId);
            }
        }

        std::set<unsigned> boneUnion;
        std::set_union(
            referencedBones.begin(), referencedBones.end(),
            thisTriangleBones.begin(), thisTriangleBones.end(),
            std::inserter(boneUnion, boneUnion.begin()));
        if (boneUnion.size() > boneLimit) {
            out.push_back(newSubmesh);
            newSubmesh = newEmptySubmeshWithVertices(*this);
            referencedBones.clear();
        }

        std::set_union(
            referencedBones.begin(), referencedBones.end(),
            thisTriangleBones.begin(), thisTriangleBones.end(),
            std::inserter(referencedBones, referencedBones.begin()));

         newSubmesh->addFace(face);
    }

    out.push_back(newSubmesh);
}

void CalCoreSubmesh::optimizeVertexCache() {
    if (m_faces.empty()) {
        return;
    }

    std::vector<Face> newFaces(m_faces.size());

    Forsyth::OptimizeFaces(
        m_faces[0].vertexId,
        3 * m_faces.size(),
        m_vertices.size(),
        newFaces[0].vertexId,
        32);

    m_faces.swap(newFaces);
}

void CalCoreSubmesh::renumberIndices() {
    // just because operator[] is invalid on an empty vector. in C++11
    // we could use .data().
    if (m_faces.empty()) {
        return;
    }

    std::vector<Face> newFaces(m_faces.size());
    size_t indexCount = m_faces.size() * 3;

    std::vector<CalIndex> mapping(m_vertices.size(), -1); // old -> new
    std::vector<bool> populated(m_vertices.size(), false); // used index
    const CalIndex* oldIndices = m_faces[0].vertexId;
    CalIndex* newIndices = newFaces[0].vertexId;
    unsigned int outputVertexCount = 0;

    for (size_t i = 0; i < indexCount; ++i) {
        CalIndex oldIndex = oldIndices[i];
        CalIndex newIndex = mapping[oldIndex];
        if (!populated[oldIndex]) {
            populated[oldIndex] = true;
            mapping[oldIndex] = static_cast<CalIndex> (outputVertexCount);
            newIndex = static_cast<CalIndex> (outputVertexCount);
            outputVertexCount++;
        }
        *newIndices = newIndex;
        newIndices++;
    }

    m_faces.swap(newFaces);

    // now that the new indices are in place, reorder the vertices

    auto oldInfluences = extractInfluenceVector(m_influences);
    assert(m_vertices.size() == m_vertexColors.size());
    assert(m_vertices.size() == oldInfluences.size());

    VectorVertex newVertices(outputVertexCount);
    std::vector<CalColor32> newColors(outputVertexCount);
    InfluenceVectorVector newInfluences(outputVertexCount);
    VectorTextureCoordinate newTexCoords(hasTextureCoordinates() ? outputVertexCount : 0); // may be empty

    for (size_t oldIndex = 0; oldIndex < mapping.size(); ++oldIndex) {
        CalIndex newIndex = mapping[oldIndex];
        if (!populated[oldIndex]) {
            continue;
        }

        newVertices[newIndex] = m_vertices[oldIndex];
        newColors[newIndex] = m_vertexColors[oldIndex];
        newInfluences[newIndex] = oldInfluences[oldIndex];
        if (!newTexCoords.empty()) {
            newTexCoords[newIndex] = m_textureCoordinates[oldIndex];
        }
    }

    MorphTargetArray newMorphTargets;

    for (size_t i = 0; i < m_morphTargets.size(); ++i) {
        const auto& mt = m_morphTargets[i];
        CalCoreMorphTarget::VertexOffsetArray newOffsets;
        for (auto vo = mt->vertexOffsets.begin(); vo != mt->vertexOffsets.end(); ++vo) {
            CalIndex newIndex = mapping[vo->vertexId];
            if (populated[vo->vertexId]) {
                newOffsets.push_back(VertexOffset(newIndex, vo->position, vo->normal));
            }
        }
        newMorphTargets.push_back(ISTD::make_shared<CalCoreMorphTarget>(mt->name, newVertices.size(), newOffsets));
    }

    m_vertices.swap(newVertices);
    m_vertexColors.swap(newColors);
    m_influences = generateInfluenceVector(newInfluences);
    m_textureCoordinates.swap(newTexCoords);
    m_morphTargets.swap(newMorphTargets);

    m_minimumVertexBufferSize = outputVertexCount;
}

void CalCoreSubmesh::normalizeNormals() {
    const float inf = std::numeric_limits<float>::infinity();
    size_t numVertices = m_vertices.size();
    for (size_t i = 0; i < numVertices; ++i) {
        CalVector4& n = m_vertices[i].normal;
        float length = n.length();
        if (length > 0.0001f && length != inf) {
            n *= 1.0f / length;
        } else {
            // We pick positive Y just because it's "up" in Northstar.  The
            // caller really shouldn't care what value gets returned, since
            // they gave us garbage.
            n = CalVector4(0.0f, 1.0f, 0.0f, 0.0f);
        }
    }
}

void CalCoreSubmesh::removeMorpTargetsWithEmptyNames() {
    std::vector<ISTD::shared_ptr<CalCoreMorphTarget>> b;

    size_t count = m_morphTargets.size();
    b.reserve(count);

    for (size_t j = 0; j < count; ++j) {
        if (!m_morphTargets[j]->name.empty()) {
            b.push_back(m_morphTargets[j]);
        }
    }

    std::swap(m_morphTargets, b);
}

void CalCoreSubmesh::sortBackToFrontByCoverage() {
    if (m_faces.empty()) {
        return;
    }

    sortTrianglesByCoverage(
        m_faces.size(),
        m_faces[0].vertexId,
        &m_vertices[0].position.x,
        sizeof(Vertex) / sizeof(float));
}

void CalCoreSubmesh::sortBackToFrontTopologically() {
    if (m_faces.empty()) {
        return;
    }

    sortTrianglesTopologically(
        m_faces.size(),
        m_faces[0].vertexId,
        &m_vertices[0].position.x,
        sizeof(Vertex) / sizeof(float));
}

void CalCoreSubmesh::sortBackToFrontByDistance() {
    if (m_faces.empty()) {
        return;
    }

    sortTrianglesByDistanceToCenter(
        m_faces.size(),
        m_faces[0].vertexId,
        &m_vertices[0].position.x,
        sizeof(Vertex) / sizeof(float));
}

CalExportedInfluences CalCoreSubmesh::exportInfluences(unsigned int influenceLimit, float influenceMargin) const {
    CalExportedInfluences outInfluenceData;

    // NOTE: May be a bit more efficient to use a sorted vector, hash map, or
    // array of bools or something.
    std::set<unsigned int> usedBoneIds;

    unsigned int maxInfluenceCount = 0;
    CalWeightsBoneIdsPair pair;
    for (auto it = m_influences.begin(); it != m_influences.end(); ++it) {
        const auto& influence = *it;

        // At least one bone has to be maintained - even if it has zero or minimal influence.
        if (pair.weights.size() < influenceLimit && (pair.weights.empty() || influence.weight > influenceMargin)) {
            pair.weights.push_back(std::max(0.0f, influence.weight));
            pair.boneIds.push_back(influence.boneId);
            usedBoneIds.insert(influence.boneId);
        }

        if (influence.lastInfluenceForThisVertex) {
            unsigned int influenceCount = pair.weights.size();
            maxInfluenceCount = std::max(maxInfluenceCount, influenceCount);

            for (unsigned int i = pair.weights.size(); i < influenceLimit; ++i) {
                pair.weights.push_back(0);
                pair.boneIds.push_back(0);
            }

            float total = 0;
            for (unsigned int i = 0; i < pair.weights.size(); ++i) {
                total += pair.weights[i];
            }
            if (std::fabs(total - 1.0) >= 0.001 && std::fabs(total) >= 0.001) {
                for (unsigned int i = 0; i < pair.weights.size(); ++i) {
                    pair.weights[i] /= total;
                }
            }

            outInfluenceData.weightsBoneIdsPairs.push_back(pair);
            pair = CalWeightsBoneIdsPair();
        }
    }
    outInfluenceData.maximumInfluenceCount = maxInfluenceCount;

    if (m_vertices.size() != outInfluenceData.weightsBoneIdsPairs.size()) {
        throw std::runtime_error("Influence count must match vertex count");
    }
    outInfluenceData.usedBoneIds = std::vector<unsigned int>(usedBoneIds.begin(), usedBoneIds.end());

    return outInfluenceData;
}
bool CalCoreSubmesh::simplifySubmesh(unsigned int target_tri_count, unsigned int quality) {
    bool retval = false;
    if(target_tri_count < m_faces.size()) {
    
        auto simplifier = new Simplify;
        simplifier->addVertices(m_vertices, m_textureCoordinates);
        simplifier->addFaces(m_faces);
        retval = simplifier->meshSimplify(target_tri_count, quality);
        // apply results to this submesh indexed triangle
        simplifier->applyFaces(m_faces);
        delete simplifier;
        
    }
    return retval;
}
    
