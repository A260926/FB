#pragma once
#include <vcg/complex/complex.h>
#include <vcg/complex/algorithms/local_optimization.h>
#include <vcg/complex/algorithms/local_optimization/tri_edge_collapse_quadric.h>
#include <vcg/complex/algorithms/create/platonic.h>
#include <iostream>
#include <stdexcept>
#include <iomanip>
using namespace vcg;
using namespace vcg::tri;
class V; class F;
struct Types : UsedTypes<Use<V>::AsVertexType,Use<F>::AsFaceType>{};
class V : public Vertex<Types,vertex::VFAdj,vertex::Coord3d,vertex::Mark,vertex::BitFlags> {
 math::Quadric<double> q;
 public: math::Quadric<double>& Qd(){return q;}
};
class F : public Face<Types,face::VFAdj,face::VertexRef,face::BitFlags>{};
class Mesh : public vcg::tri::TriMesh<std::vector<V>,std::vector<F>>{};
using Pair=BasicVertexPair<V>;
class Collapse : public TriEdgeCollapseQuadric<Mesh,Pair,Collapse,QInfoStandard<V>>{
 using Base=TriEdgeCollapseQuadric<Mesh,Pair,Collapse,QInfoStandard<V>>;
 public: Collapse(const Pair&p,int i,BaseParameterClass*pp):Base(p,i,pp){}
};
inline double signed_volume(const Mesh&m){
 double v=0; for(const auto&f:m.face)if(!f.IsD())v+=f.cP(0).dot(f.cP(1)^f.cP(2))/6.; return v;
}
inline void make_fixture(Mesh&m){
 Sphere(m,1);
 for(auto&v:m.vert){auto p=v.P();v.P()=Point3d(1.7*p[0]+.13*p[1]*p[2],.8*p[1],1.2*p[2]+.17*p[0]*p[1]);}
 UpdateTopology<Mesh>::VertexFace(m); UpdateFlags<Mesh>::FaceBorderFromNone(m);UpdateBounding<Mesh>::Box(m);
}
