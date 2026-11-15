#include <GEL/HMesh/Manifold.h>
#include <GEL/HMesh/obj_load.h>
#include <GEL/HMesh/obj_save.h>
#include <GEL/HMesh/AttributeVector.h>
#include <GEL/HMesh/triangulate.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace HMesh;
using CGLA::Vec3d;
static void require(bool b) { if(!b) throw std::runtime_error("GEL baseline check failed"); }
static int boundaries(const Manifold& m) {int n=0;for(auto h:m.halfedges())if(m.walker(h).face()==InvalidFaceID)++n;return n;}
int main() {
 const std::string input=".task-build/starting.obj",output=".task-build/roundtrip.obj";
 {
  std::ofstream f(input);require(bool(f));
  f<<"v 0 0 0\nv 2 0 0\nv 0 2 0\nv 0 0 2\n"
   <<"vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 2\n"
   <<"f 1/1/1 2/2/1 4/3/1\nf 2/1/1 3/2/1 4/3/1\nf 3/1/1 1/2/1 4/3/1\n";
 }
 Manifold m;require(obj_load(input,m));require(valid(m));
 require(m.no_vertices()==4&&m.no_faces()==3&&boundaries(m)==3);
 std::cout<<"OPEN_TETRA vertices=4 faces=3 boundary_halfedges=3\n";
 HalfEdgeID bh;for(auto h:m.halfedges())if(m.walker(h).face()==InvalidFaceID){bh=h;break;}
 FaceID cap=m.close_hole(bh);require(m.in_use(cap));require(boundaries(m)==0&&m.no_faces()==4&&valid(m));
 VertexID center=m.split_face_by_vertex(cap);require(m.in_use(center));require(m.no_vertices()==5&&m.no_faces()==6&&valid(m));
 std::cout<<"CLOSE_AND_SPLIT vertices=5 faces=6 boundary_halfedges=0\n";
 HalfEdgeID edge=*m.halfedges_begin();m.split_edge(edge);require(valid(m)&&m.no_vertices()==6);
 HalfEdgeAttributeVector<int> tags(-1);for(auto h:m.halfedges())tags[h]=int(h.get_index())+101;
 IDRemap remap;m.cleanup(remap);tags.cleanup(remap.hmap);
 for(auto entry:remap.hmap) require(tags[entry.second]==int(entry.first.get_index())+101);
 require(valid(m));std::cout<<"CLEANUP vertices=6 tags_remapped=1\n";
 require(obj_save(output,m));Manifold back;require(obj_load(output,back));
 require(valid(back)&&back.no_vertices()==m.no_vertices()&&back.no_faces()==m.no_faces()&&boundaries(back)==0);
 std::ifstream saved(output);std::string line;int vt=0,vn=0;while(std::getline(saved,line)){vt+=line.rfind("vt ",0)==0;vn+=line.rfind("vn ",0)==0;}
 require(vt==0&&vn==0);std::cout<<"LEGACY_OBJ vt=0 vn=0 topology_roundtrip=1\nGEL_BASELINE_OK\n";
}
