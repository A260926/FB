#include "libHh/PMesh.h"
#include "libHh/SrMesh.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
using namespace hh;
static void check(bool b,const char* where){if(!b)throw std::runtime_error(where);}
static std::vector<std::array<float,3>> points(const GMesh& m){
 std::vector<std::array<float,3>> p;
 for(Vertex v:m.vertices()){const auto& q=m.point(v);p.push_back({q[0],q[1],q[2]});}
 std::sort(p.begin(),p.end());return p;
}
int main(){
 std::ifstream pmfile("demos/data/imagesquare.pm",std::ios::binary);check(bool(pmfile),"PM file");PMesh pm;pm.read(pmfile);
 PMeshRStream r(pm);PMeshIter it(r);const int base=it._vertices.num();int steps=0;
 while(it.next())++steps;it.ok();const int full=it._vertices.num();check(full==base+steps,"PM forward count");
 int back=0;while(it.prev())++back;check(back==steps&&it._vertices.num()==base,"PM backward count");
 auto pm_mesh=it.extract_gmesh();pm_mesh.ok();check(pm_mesh.num_faces()==2,"PM geometry");
 std::cout<<"PM_ITER imagesquare base="<<base<<" splits="<<steps<<" faces="<<pm_mesh.num_faces()<<std::endl;
 std::ifstream file("demos/data/gcanyon-prefix.srm",std::ios::binary);check(bool(file),"SR file");SrMesh sr;sr.read_srm(file);sr.ok();
 const int srbase=sr.num_active_vertices();sr.fully_refine();sr.ok();auto m=sr.extract_gmesh();m.ok();
 check(m.num_vertices()==srbase+128,"SR real splits");const auto expected=points(m);const int faces=m.num_faces();
 sr.fully_coarsen();sr.ok();check(sr.num_active_vertices()==srbase,"SR coarse count");
 std::stringstream bytes(std::ios::in|std::ios::out|std::ios::binary);sr.write_srm(bytes);
 SrMesh restored;restored.read_srm(bytes);restored.fully_refine();restored.ok();auto m2=restored.extract_gmesh();m2.ok();
 check(m2.num_vertices()==srbase+128&&m2.num_faces()==faces&&points(m2)==expected,"SR roundtrip geometry");
 std::cout<<"SR_PREFIX base="<<srbase<<" splits="<<m.num_vertices()-srbase<<" full="<<m.num_vertices()<<" faces="<<faces<<" srm_bytes="<<bytes.str().size()<<std::endl;
 std::cout<<"PM_SR_BASELINE_OK\n";
}
