//****************************************************************************//
//  simplify.cpp                                                              //
//  Mesh Polygon Reduction Algorithm from Progressive Mesh code               //
//  by Stan Melax (c) 1998                                                    //
//  Permission to use any of this code wherever you want is granted.          //
//  Although, please do acknowledge authorship if appropriate.                //
//****************************************************************************//
// Ported to STL & Cal3D by IMVU (c) 2014.                                    //
// Progressive permutation removed by IMVU (c) 2014.                          //
// Texture derivative enhancements added by IMVU (c) 2014.                    //
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

/*
 *  For the polygon reduction algorithm we use data structures
 *  that contain a little bit more information than the usual
 *  indexed face set type of data structure.
 *  From a vertex we wish to be able to quickly get the
 *  neighbouring faces and vertices.
 */

Simplify::Triangle::Triangle(std::vector<reduxVertex *> &vertices, std::vector<Triangle *> &triangles, reduxVertex *v0, reduxVertex *v1, reduxVertex *v2)
    : parent_vertices(vertices)
    , parent_triangles(triangles)
{
    vertex[0] = v0;
    vertex[1] = v1;
    vertex[2] = v2;
    computeNormal();
    computeTB();
    for (int i = 0; i<3; i++) {
        vertex[i]->face.push_back(this);
        for (int j = 0; j<3; j++) if (i != j) {
            VECTOR_ADD_UNIQUE(vertex[i]->neighbor, vertex[j]);
        }
    }
}

Simplify::Triangle::~Triangle(){
    int i;
    VECTOR_REMOVE_VALUE(parent_triangles, this);
    for (i = 0; i<3; i++) {
        if (vertex[i])
        {
            VECTOR_REMOVE_VALUE(vertex[i]->face, this);
        }
    }
    for (i = 0; i<3; i++) {
        int i2 = (i + 1) % 3;
        if (!vertex[i] || !vertex[i2]) continue;
        vertex[i]->removeIfNonNeighbor(vertex[i2]);
        vertex[i2]->removeIfNonNeighbor(vertex[i]);
    }
}
int Simplify::Triangle::hasVertex(reduxVertex *v) {
    return (v == vertex[0] || v == vertex[1] || v == vertex[2]);
}
void Simplify::Triangle::computeNormal(){
    CalVector v0 = vertex[0]->position;
    CalVector v1 = vertex[1]->position;
    CalVector v2 = vertex[2]->position;
    normal = cross((v1 - v0), (v2 - v1));
    normal.normalize();
}
void Simplify::Triangle::computeTB(){
    //Calculate Tangent and Bitangent (AKA binormal)

    float dx1 = vertex[1]->position.x - vertex[0]->position.x;
    float dx2 = vertex[2]->position.x - vertex[0]->position.x;
    float dy1 = vertex[1]->position.y - vertex[0]->position.y;
    float dy2 = vertex[2]->position.y - vertex[0]->position.y;
    float dz1 = vertex[1]->position.z - vertex[0]->position.z;
    float dz2 = vertex[2]->position.z - vertex[0]->position.z;
    
    float du1 = vertex[1]->u - vertex[0]->u;
    float du2 = vertex[2]->u - vertex[0]->u;
    float dv1 = vertex[1]->u - vertex[0]->v;
    float dv2 = vertex[2]->v - vertex[0]->v;
        
    float recip = 1.0f / (du1 * dv2 - du2 * dv1);
    T = CalVector((dv2 * dx1 - dv1 * dx2) * recip, (dv2 * dy1 - dv1 * dy2) * recip, (dv2 * dz1 - dv1 * dz2) * recip);
    B = CalVector((du1 * dx2 - du2 * dx1) * recip, (du1 * dy2 - du2 * dy1) * recip, (du1 * dz2 - du2 * dz1) * recip);
    // this means dot product later only accounts for angular changes
    // in texture derivative, that'll do for now
    T.normalize();
    B.normalize();
}
void Simplify::Triangle::replaceVertex(reduxVertex *vold, reduxVertex *vnew) {
    assert(vold && vnew);
    assert(vold == vertex[0] || vold == vertex[1] || vold == vertex[2]);
    assert(vnew != vertex[0] && vnew != vertex[1] && vnew != vertex[2]);
    if (vold == vertex[0]){
        vertex[0] = vnew;
    }
    else if (vold == vertex[1]){
        vertex[1] = vnew;
    }
    else {
        assert(vold == vertex[2]);
        vertex[2] = vnew;
    }
    int i;
    VECTOR_REMOVE_VALUE(vold->face, this);
    assert(!VECTOR_CONTAINS(vnew->face, this));
    vnew->face.push_back(this);
    for (i = 0; i<3; i++) {
        vold->removeIfNonNeighbor(vertex[i]);
        vertex[i]->removeIfNonNeighbor(vold);
    }
    for (i = 0; i<3; i++) {
        assert(VECTOR_CONTAINS(vertex[i]->face, this));
        for (int j = 0; j<3; j++) if (i != j) {
            VECTOR_ADD_UNIQUE(vertex[i]->neighbor, vertex[j]);
        }
    }
    computeNormal();
    computeTB();
}

Simplify::reduxVertex::reduxVertex(std::vector<reduxVertex *> &vertices, const CalPoint4 &p, int _id)
    : parent_vertices(vertices)
{
    position.x = p.x;
    position.y = p.y;
    position.z = p.z;
    u = v = 0.0f;
    id = _id;
}
Simplify::reduxVertex::reduxVertex(std::vector<reduxVertex *> &vertices, const CalPoint4 &p, float _u, float _v, int _id)
    : parent_vertices(vertices)
{
    position.x = p.x;
    position.y = p.y;
    position.z = p.z;
    u = _u;
    v = _v;
    id = _id;
}

Simplify::reduxVertex::~reduxVertex(){
    assert(face.size() == 0);
    while (neighbor.size()) {
        VECTOR_REMOVE_VALUE(neighbor[0]->neighbor, this);
        VECTOR_REMOVE_VALUE(neighbor, neighbor[0]);
    }
    VECTOR_REMOVE_VALUE(parent_vertices, this);
}

bool Simplify::reduxVertex::isBorder() {
    unsigned int i, j;
    
    for(i=0; i< neighbor.size(); i++) {
        int count = 0;
        for(j=0; j< face.size(); j++) {
            if(face[j]->hasVertex(neighbor[i])) {
                count++;
            }
        }
        assert(count > 0);
        if(count==1) {
            return true;
        }
    }
    return false;
}

void Simplify::reduxVertex::removeIfNonNeighbor(reduxVertex *n) {
    // removes n from neighbor list if n isn't a neighbor.
    if (!VECTOR_CONTAINS(neighbor, n)) return;
    for (unsigned int i = 0; i<face.size(); i++) {
        if (face[i]->hasVertex(n)) return;
    }
    VECTOR_REMOVE_VALUE(neighbor, n);
}

float Simplify::computeEdgeCollapseCost(reduxVertex *u, reduxVertex *v) {
    // if we collapse edge uv by moving u to v then how 
    // much different will the model change, i.e. how much "error".
    // Texture, vertex normal, and border vertex code was removed
    // to keep this demo as simple as possible.
    // The method of determining cost was designed in order 
    // to exploit small and coplanar regions for
    // effective polygon reduction.
    // Is is possible to add some checks here to see if "folds"
    // would be generated.  i.e. normal of a remaining face gets
    // flipped.  I never seemed to run into this problem and
    // therefore never added code to detect this case.
    unsigned int i;
    float edgelength = (v->position - u->position).length();
    float curvature = 0;

    // find the "sides" triangles that are on the edge uv
    std::vector<Triangle *> sides;
    for (i = 0; i<u->face.size(); i++) {
        if (u->face[i]->hasVertex(v)){
            sides.push_back(u->face[i]);
        }
    }

    // use the triangle facing most away from the sides 
    // to determine our curvature term
    for (i = 0; i<u->face.size(); i++) {
        float mincurv = 1.0f; // curve for face i and closer side to it
        for (unsigned int j = 0; j<sides.size(); j++) {
            // use dot product of face normals.
            float dotprod = dot(u->face[i]->normal, sides[j]->normal);
            float dotprodT = dot(u->face[i]->T, sides[j]->T);
            float dotprodB = dot(u->face[i]->B, sides[j]->B);
            mincurv = std::min(mincurv, (1.0f - dotprodT) / 8.0f + (1.0f - dotprodB) / 8.0f + (1.0f - dotprod) / 2.0f);
        }
        curvature = std::max(curvature, mincurv);
    }
    
    // the more coplanar the lower the curvature term 
    // but adjust for border adjacency
    bool uedge = u->isBorder();
    bool vedge = v->isBorder();
    if(u->face.size() == 1) {
        curvature += 5.0;
        m_isolateds++;
    }
    else if (u->face.size() == 2) {
        if( matchFaces(u->face[0], u->face[1]) ) {
            curvature += 5.0;
            m_isolateds++;
        }
    }
    if(uedge) {
        curvature += 4.5;
        if(vedge) {
            curvature -= 2.75;
        }
    }
    return edgelength * curvature;
}

bool Simplify::matchFaces(Triangle *t0, Triangle *t1)
{
    if( (t0->vertex[0] == t1->vertex[0] || t0->vertex[0] == t1->vertex[1] || t0->vertex[0] == t1->vertex[2]) &&
        (t0->vertex[1] == t1->vertex[0] || t0->vertex[1] == t1->vertex[1] || t0->vertex[1] == t1->vertex[2]) &&
        (t0->vertex[2] == t1->vertex[0] || t0->vertex[2] == t1->vertex[1] || t0->vertex[2] == t1->vertex[2]) ) {
        return true;
    }
    return false;
}

bool Simplify::computeEdgeCostAtVertex(reduxVertex *v) {
    // compute the edge collapse cost for all edges that start
    // from vertex v.  Since we are only interested in reducing
    // the object by selecting the min cost edge at each step, we
    // only cache the cost of the least cost edge at this vertex
    // (in member variable collapse) as well as the value of the 
    // cost (in member variable objdist).
    if (v->neighbor.size() == 0) {
        // v doesn't have neighbors so it costs nothing to collapse
        v->collapse = NULL;
        v->objdist = -0.01f;
        return true;
    }
    v->objdist = 1000000;
    v->collapse = NULL;
    if(v->neighbor.size() > 25) {
        return false; // abort
    }
    // search all neighboring edges for "least cost" edge
    for (unsigned int i = 0; i<v->neighbor.size(); i++) {
        float dist;
        dist = computeEdgeCollapseCost(v, v->neighbor[i]);
        if (dist<v->objdist) {
            v->collapse = v->neighbor[i];  // candidate for edge collapse
            v->objdist = dist;             // cost of the collapse
        }
    }
    return true;
}

bool Simplify::computeAllEdgeCollapseCosts() {
    // For all the edges, compute the difference it would make
    // to the model if it was collapsed.  The least of these
    // per vertex is cached in each vertex object.
    for (unsigned int i = 0; i<vertices.size(); i++) {
        if(! computeEdgeCostAtVertex(vertices[i])) {
            return false; // abort
        }
    }
    return true;
}

void Simplify::collapse(reduxVertex *u, reduxVertex *v){
    // Collapse the edge uv by moving vertex u onto v
    // Actually remove tris on uv, then update tris that
    // have u to have v, and then remove u.
    if (!v) {
        // u is a vertex all by itself so just delete it
        delete u;
        return;
    }
    int i;
    std::vector<reduxVertex *>tmp;
    // make tmp a list of all the neighbors of u
    for (i = 0; i<static_cast<int>(u->neighbor.size()); i++) {
        tmp.push_back(u->neighbor[i]);
    }
    // delete triangles on edge uv:
    for (i = static_cast<int>(u->face.size()) - 1; i >= 0; i--) {
        if (u->face[i]->hasVertex(v)) {
            delete(u->face[i]);
        }
    }
    // update remaining triangles to have v instead of u
    for (i = static_cast<int>(u->face.size()) - 1; i >= 0; i--) {
        u->face[i]->replaceVertex(u, v);
    }
    delete u;
    // recompute the edge collapse costs for neighboring vertices
    for (i = 0; i<static_cast<int>(tmp.size()); i++) {
        computeEdgeCostAtVertex(tmp[i]);
    }
}

void Simplify::addVertices(const CalCoreSubmesh::VectorVertex &vert, const CalCoreSubmesh::VectorTextureCoordinate &tex){
    vertices.clear();
    if(tex.size() == vert.size()) {
        for (unsigned int i = 0; i<vert.size(); i++) {
            vertices.push_back(new reduxVertex(vertices, vert[i].position, tex[i].u, tex[i].v, i));
        }
    }
    else
    { 
        for (unsigned int i = 0; i<vert.size(); i++) {
            vertices.push_back(new reduxVertex(vertices, vert[i].position, i));
        }
    }
}

void Simplify::addFaces(const CalCoreSubmesh::VectorFace &tri){
    triangles.clear();
    for (unsigned int i = 0; i<tri.size(); i++) {
        if(!(tri[i].vertexId[0] == tri[i].vertexId[1] ||
             tri[i].vertexId[1] == tri[i].vertexId[2] ||
             tri[i].vertexId[2] == tri[i].vertexId[0])) {
                    
                triangles.push_back(
                    new Triangle(vertices, triangles,
                        vertices[tri[i].vertexId[0]],
                        vertices[tri[i].vertexId[1]],
                        vertices[tri[i].vertexId[2]]));
        }
    }
}

void Simplify::applyFaces(CalCoreSubmesh::VectorFace &tris){
    int gone = tris.size() - triangles.size();
    if(gone >0){
        tris.clear();
        tris.reserve(triangles.size());
        for (unsigned int i = 0; i<triangles.size(); i++) {
            tris.push_back( CalCoreSubmesh::Face(   triangles[i]->vertex[0]->id,
                                                    triangles[i]->vertex[1]->id,
                                                    triangles[i]->vertex[2]->id ));
        }
    }
}

Simplify::reduxVertex *Simplify::MinimumCostEdge(){
    // Find the edge that when collapsed will affect model the least.
    // This funtion actually returns a reduxVertex, the second vertex
    // of the edge (collapse candidate) is stored in the vertex data.
    // Serious optimization opportunity here: this function currently
    // does a sequential search through an unsorted list :-(
    // Our algorithm could be O(n*lg(n)) instead of O(n*n)
    Simplify::reduxVertex *mn = vertices[0];
    for (unsigned int i = 0; i<vertices.size(); i++) {
        if (vertices[i]->objdist < mn->objdist) {
            mn = vertices[i];
        }
    }
    return mn;
}

bool Simplify::meshSimplify(unsigned int target_tri_count, unsigned int quality) {
    bool retval = false;
    if(target_tri_count < triangles.size()) {
        float lastError, error;
        m_isolateds = 0;
        
        lastError = error = (100.0f-quality ) * 0.00001f;

        if (computeAllEdgeCollapseCosts()) { // cache all edge collapse costs
            if(m_isolateds / static_cast<float>(triangles.size()) < 0.4f) {
                if(m_isolateds / static_cast<float>(triangles.size()) > 0.21f) { // boost poly count if isolateds are high
                    target_tri_count += ( triangles.size() - target_tri_count) /2;
                }
                while (triangles.size() > target_tri_count || vertices.size() > 65535 ||
                      ( lastError < error && triangles.size() > (target_tri_count/2)) ) {
                    // get the next vertex to collapse
                    reduxVertex *mn = MinimumCostEdge();
                    lastError = mn->objdist;
                    collapse(mn, mn->collapse);
                }
                retval = true;
            }
        }
    }
    return retval;
}

Simplify::~Simplify() {        // cleanup memory
        //while (triangles.size() > 0) {
            // get the next vertex to collapse
        //    reduxVertex *mn = MinimumCostEdge();
        //    collapse(mn, mn->collapse);
        //}
        for(unsigned int i = 0; i < triangles.size(); i++)
        {
            delete triangles[i];
        }
        vertices.clear();
        triangles.clear();
}
 
