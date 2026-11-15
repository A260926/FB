#include "libHh/GMesh.h"
#include "libHh/MeshOp.h"
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
using namespace hh;
static void check(bool b) { if(!b) throw std::runtime_error("baseline check failed"); }
int main() {
 GMesh m; const int n=6,k=5; std::vector<Vertex> v;
 for(int i=0;i<n;++i) for(int j=0;j<k;++j) {
  const double u=6.283185307179586*i/n,w=6.283185307179586*j/k;
  Vertex p=m.create_vertex(); v.push_back(p);
  m.set_point(p,Point(float((3+.7*std::cos(w))*std::cos(u)),float((3+.7*std::cos(w))*std::sin(u)),float(.7*std::sin(w))));
 }
 auto at=[&](int i,int j){return v[(i%n)*k+j%k];};
 for(int i=0;i<n;++i) for(int j=0;j<k;++j) {
  m.create_face(at(i,j),at(i+1,j),at(i+1,j+1));
  m.create_face(at(i,j),at(i+1,j+1),at(i,j+1));
 }
 m.ok(); check(m.num_vertices()==30&&m.num_edges()==90&&m.num_faces()==60); check(mesh_genus(m)==1);
 for(Edge e:m.edges()) check(m.face1(e)&&m.face2(e));
 Face f=m.id_face(1); Corner c=m.corner(m.vertex(f,0),f);
 m.set_string(c,"uv=(0.25 0.75) normal=(0 0 2) marker=left");
 std::stringstream ss;m.write(ss);GMesh copy;copy.read(ss);copy.ok();
 auto cf=copy.id_face(1); auto cc=copy.corner(copy.vertex(cf,0),cf);
 float uv[2]={};check(copy.parse_corner_key_vec(cc,"uv",uv));check(uv[0]==.25f&&uv[1]==.75f);
 float normal[3]={};check(copy.parse_corner_key_vec(cc,"normal",normal));check(normal[2]==2);
 std::cout<<"TORUS vertices=30 edges=90 faces=60 genus=1\n";
 std::cout<<"CORNER_IO uv=0.25,0.75 normal_z=2\n";
 copy.split_edge(*copy.edges().begin());copy.ok();
 check(copy.num_vertices()==31&&copy.num_edges()==93&&copy.num_faces()==62&&mesh_genus(copy)==1);
 std::cout<<"SPLIT vertices=31 edges=93 faces=62 genus=1\nHH_BASELINE_OK\n";
}
