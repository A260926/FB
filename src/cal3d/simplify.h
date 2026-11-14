//****************************************************************************//
//  simplify.h                                                                //
//  Mesh Polygon Reduction Algorithm from Progressive Mesh code               //
//  by Stan Melax (c) 1998                                                    //
//  Permission to use any of this code wherever you want is granted.          //
//  Although, please do acknowledge authorship if appropriate.                //
//****************************************************************************//
// Ported to STL & Cal3D by IMVU (c) 2014.                                    //
// Progressive permutation removed by IMVU (c) 2014.                          //
// Texture derivative enhancements added by IMVU (c) 2014.                    //
//****************************************************************************//

#pragma once

#include <ostream>
#include <set>
#include <map>
#include "cal3d/BoostToStd.h"
#include "cal3d/aabox.h"
#include "cal3d/color.h"
#include "cal3d/global.h"
#include "cal3d/memory.h"
#include "cal3d/vector.h"
#include "cal3d/vector4.h"
#include "cal3d/bonetransform.h"
#include "cal3d/coresubmesh.h"


#define VECTOR_CONTAINS(a, b)       (std::find(a.begin(), a.end(), b)!=a.end())
#define VECTOR_REMOVE_VALUE(a, b)   (a.erase(std::find(a.begin(), a.end(), b)))
#define VECTOR_ADD_UNIQUE(a, b)     do {  if(std::find(a.begin(), a.end(), b)==a.end())  a.push_back(b); } while(0)

class CAL3D_API Simplify {

    struct Triangle;
    struct reduxVertex;

    struct reduxVertex {
    public:
        CalVector       position; // location of point in euclidean space
        float           u, v; // texture coordinates
        int             id;       // place of vertex in original list
        std::vector<reduxVertex *>   neighbor; // adjacent vertices
        std::vector<Triangle *> face;     // adjacent triangles
        float            objdist;  // cached cost of collapsing edge
        reduxVertex *         collapse; // candidate vertex for collapse
        std::vector<reduxVertex *> &parent_vertices;
        bool isBorder(); // test if vertex is on border
        reduxVertex(std::vector<reduxVertex *> &vertices, const CalPoint4 &p, int _id);
        reduxVertex(std::vector<reduxVertex *> &vertices, const CalPoint4 &p, float _u, float _v, int _id);
        ~reduxVertex();
        void             removeIfNonNeighbor(reduxVertex *n);
    };

    struct Triangle {
    public:
        reduxVertex *         vertex[3]; // the 3 points that make this tri
        CalVector        normal;    // unit vector othogonal to this face
        CalVector        T;    // vector tangent (delta u)
        CalVector        B;    // vector bitangent (delta v)
        std::vector<reduxVertex *> &parent_vertices;
        std::vector<Triangle *> &parent_triangles;
        Triangle(std::vector<reduxVertex *> &vertices, std::vector<Triangle *> &triangles, reduxVertex *v0, reduxVertex *v1, reduxVertex *v2);
        ~Triangle();
        void             computeNormal();
        void             computeTB();
        void             replaceVertex(reduxVertex *vold, reduxVertex *vnew);
        int              hasVertex(reduxVertex *v);
    };
    
public:

    ~Simplify();
    void addFaces(const CalCoreSubmesh::VectorFace &tri);
    void addVertices(const CalCoreSubmesh::VectorVertex &vert, const CalCoreSubmesh::VectorTextureCoordinate &tex);
    bool meshSimplify(unsigned int tri_count, unsigned int quality);
    void applyFaces(CalCoreSubmesh::VectorFace &tris);
    
private:
    int m_isolateds;
    std::vector<reduxVertex *> vertices;
    std::vector<Triangle *> triangles;

    // internal simplification prototypes
    float computeEdgeCollapseCost(reduxVertex *u, reduxVertex *v);
    bool computeEdgeCostAtVertex(reduxVertex *v);
    bool computeAllEdgeCollapseCosts();
    void collapse(reduxVertex *u, reduxVertex *v);
    bool matchFaces(Triangle *t0, Triangle *t1);
    reduxVertex *MinimumCostEdge();
};

