#include <OpenMesh/Core/IO/MeshIO.hh>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using Mesh=OpenMesh::TriMesh_ArrayKernelT<>;
static void require(bool x,const char* msg){if(!x)throw std::runtime_error(msg);}
static int rings(const Mesh& m){
  std::vector<bool> seen(m.n_halfedges(),false);int count=0;
  for(auto h=m.halfedges_begin();h!=m.halfedges_end();++h)if(m.is_boundary(*h)&&!seen[h->idx()]){
    ++count;auto p=*h;size_t steps=0;
    do{require(m.is_boundary(p),"boundary face");require(!seen[p.idx()],"boundary repeat");seen[p.idx()]=true;
       p=m.next_halfedge_handle(p);require(++steps<=m.n_halfedges(),"boundary cycle");}while(p!=*h);
  }return count;
}
int main(){try{
  Mesh m;m.request_vertex_status();m.request_edge_status();m.request_face_status();
  OpenMesh::HPropHandleT<OpenMesh::Vec2f> uv; m.add_property(uv,"bridge_uv");m.property(uv).set_persistent(true);
  OpenMesh::FPropHandleT<int> mat;m.add_property(mat,"material");m.property(mat).set_persistent(true);
  for(int layer=0;layer<2;++layer){int n=4+layer;std::vector<Mesh::VertexHandle> loop;
    for(int i=0;i<n;++i){double a=6.283185307179586*i/n;loop.push_back(m.add_vertex(Mesh::Point(float(cos(a)),float(sin(a)),float(2*layer))));}
    auto c=m.add_vertex(Mesh::Point(0,0,float(2*layer)));
    for(int i=0;i<n;++i){auto f=layer?m.add_face(c,loop[i],loop[(i+1)%n]):m.add_face(c,loop[(i+1)%n],loop[i]);require(f.is_valid(),"cap face");m.property(mat,f)=70+layer;}
  }
  require(m.n_vertices()==11&&m.n_edges()==18&&m.n_faces()==9&&rings(m)==2,"caps");
  auto f=Mesh::FaceHandle(0);Mesh::Point p(0,0,0);for(auto v=m.cfv_iter(f);v.is_valid();++v)p+=m.point(*v);p/=3;
  m.split_copy(f,m.add_vertex(p));
  require(m.n_vertices()==12&&m.n_edges()==21&&m.n_faces()==11,"split counts");
  for(auto h=m.halfedges_begin();h!=m.halfedges_end();++h)m.property(uv,*h)=OpenMesh::Vec2f(float(h->idx())/7,-float(h->idx())/11);
  m.garbage_collection();require(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check(),"checker");
  require(rings(m)==2,"split boundaries");
  const char* path=".task-build/baseline.om";require(OpenMesh::IO::write_mesh(m,path,OpenMesh::IO::Options::Binary),"write OM");
  Mesh out;OpenMesh::HPropHandleT<OpenMesh::Vec2f> uv2;out.add_property(uv2,"bridge_uv");
  OpenMesh::FPropHandleT<int> mat2;out.add_property(mat2,"material");
  require(OpenMesh::IO::read_mesh(out,path),"read OM");require(out.n_vertices()==12&&out.n_faces()==11&&rings(out)==2,"roundtrip topology");
  for(auto h=m.halfedges_begin();h!=m.halfedges_end();++h)require((m.property(uv,*h)-out.property(uv2,*h)).norm()<1e-7,"roundtrip UV");
  for(auto it=m.faces_begin();it!=m.faces_end();++it)require(m.property(mat,*it)==out.property(mat2,*it),"roundtrip material");
  require(OpenMesh::Utils::MeshCheckerT<Mesh>(out).check(),"roundtrip checker");
  std::cout<<"BASELINE_OPENMESH split_copy V=12 E=21 F=11 boundary_loops=2\nOM_UV_MATERIAL_ROUNDTRIP_OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
