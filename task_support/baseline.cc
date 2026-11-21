#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cassert>
#include <cmath>
#include <memory>
#include <iostream>
using namespace trimesh;
static double area(const TriMesh& m){
 double a=0;for(const auto&f:m.faces)
  a+=.5*len((m.vertices[f[1]]-m.vertices[f[0]]) CROSS
             (m.vertices[f[2]]-m.vertices[f[0]]));
 return a;
}
int main(){
 TriMesh::set_verbose(0);
 std::unique_ptr<TriMesh> m(TriMesh::read("task_support/disk.ply"));
 assert(m && m->vertices.size()==4 && m->faces.size()==2);
 m->colors.assign(4,Color(.2f,.5f,.7f));m->confidences.assign(4,.75f);
 m->need_across_edge();m->need_normals();m->need_pointareas();
 int shared=0;for(const auto&f:m->across_edge)for(int i=0;i<3;++i)shared+=f[i]>=0;
 assert(shared==2);
 edgeflip(m.get());assert(m->faces.size()==2);
 double oldarea=area(*m);
 subdiv(m.get(),SUBDIV_PLANAR);
 assert(m->vertices.size()==9 && m->faces.size()==8);
 assert(m->colors.size()==9 && m->confidences.size()==9);
 assert(std::fabs(area(*m)-oldarea)<1e-5);
 m->need_normals();m->need_pointareas();m->need_across_edge();
 assert(m->normals.size()==9 && m->pointareas.size()==9);
 assert(m->write("ascii:.task-build/baseline.ply"));
 std::unique_ptr<TriMesh> r(TriMesh::read(".task-build/baseline.ply"));
 assert(r && r->vertices.size()==9 && r->faces.size()==8);
 std::cout<<"TOPOLOGY initial_shared_halfedges="<<shared<<"\n";
 std::cout<<"SUBDIV vertices="<<r->vertices.size()<<" faces="<<r->faces.size()<<" area="<<area(*r)<<"\n";
 std::cout<<"PLY_ROUNDTRIP_COMPLETE\nBASELINE_COMPLETE\n";
}
