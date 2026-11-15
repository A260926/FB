#include "pmp/surface_mesh.h"
#include "pmp/io/read_obj.h"
#include "pmp/io/write_obj.h"
#include "pmp/algorithms/normals.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace pmp;
static void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
static void connectivity(const SurfaceMesh& m){
 for(auto h:m.halfedges()){
  require(m.opposite_halfedge(m.opposite_halfedge(h))==h,"opposite");
  require(m.next_halfedge(m.prev_halfedge(h))==h,"next-prev");
 }
 for(auto f:m.faces())require(m.valence(f)==3,"triangle");
}
int main(){try{
 const char* path=".task-build/input.obj";std::ofstream out(path);
 out<<"v 0 0 0\nv 2 0 0\nv 0 1 0\nv 0 -1 1\n"
    <<"vt -2 3\nvt 4 5\nvt 0 8\nvt 10 -4\nvt 12 6\nvt 9 7\n"
    <<"vn 0 0 1\ns 7\nf 1/1/1 2/2/1 3/3/1\ns 9\nf 2/4 1/5 4/6\n";out.close();
 SurfaceMesh m;read_obj(m,path);require(m.n_vertices()==4&&m.n_edges()==5&&m.n_faces()==2,"OBJ topology");
 auto tex=m.get_halfedge_property<TexCoord>("h:tex");require(bool(tex),"OBJ texture");
 std::cout<<"START_OBJ_RECORDS halfedge_normals="<<bool(m.get_halfedge_property<Normal>("h:normal"))
          <<" smoothing_groups="<<bool(m.get_face_property<int>("f:smoothing_group"))<<'\n';
 vertex_normals(m);face_normals(m);
 for(auto v:m.vertices()){auto n=m.get_vertex_property<Normal>("v:normal")[v];require(std::isfinite(n[0])&&norm(n)>0.99,"normal");}
 require(norm(face_normal(m,Face(0))-Normal(0,0,1))<1e-6,"first face normal");
 IOFlags flags;flags.use_halfedge_texcoords=true;flags.use_vertex_normals=true;
 write_obj(m,".task-build/output.obj",flags);SurfaceMesh back;read_obj(back,".task-build/output.obj");
 require(back.n_faces()==2&&back.n_vertices()==4,"OBJ roundtrip");
 auto tex2=back.get_halfedge_property<TexCoord>("h:tex");require(bool(tex2),"roundtrip tex");
 for(auto f:m.faces()){
  for(auto h:m.halfedges(f)){
   Vertex from=m.from_vertex(h),to=m.to_vertex(h);auto other=back.find_halfedge(from,to);
   require(other.is_valid()&&norm(tex[h]-tex2[other])<1e-6,"roundtrip source-side UV");
  }
 }
 auto h=m.find_halfedge(Vertex(0),Vertex(1));require(h.is_valid(),"split edge");
 const Vertex v=m.add_vertex(m.position(Vertex(0))*Scalar(0.7)+m.position(Vertex(1))*Scalar(0.3));
 m.split(m.edge(h),v);require(m.n_vertices()==5&&m.n_edges()==8&&m.n_faces()==4,"split topology");
 auto unused=m.add_vertex(Point(7,8,9));m.delete_vertex(unused);m.garbage_collection();
 vertex_normals(m);face_normals(m);connectivity(m);connectivity(back);
 require(m.n_vertices()==5&&m.n_faces()==4,"garbage collection");
 std::cout<<"BASELINE_PMP OBJ_UV_ROUNDTRIP_OK\nPRODUCTION_SPLIT_GC V=5 E=8 F=4 NORMALS_OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
