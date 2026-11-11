#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>
#include <cassert>
#include <iostream>
using Mesh=OpenMesh::TriMesh_ArrayKernelT<>;
int main(){
 Mesh m;m.request_halfedge_texcoords2D();
 Mesh::VertexHandle v[9];
 for(int y=0;y<3;++y)for(int x=0;x<3;++x)v[y*3+x]=m.add_vertex(Mesh::Point(x,y,0));
 for(int y=0;y<2;++y)for(int x=0;x<2;++x){
  int i=y*3+x;
  assert(m.add_face(v[i],v[i+1],v[i+4]).is_valid());
  assert(m.add_face(v[i],v[i+4],v[i+3]).is_valid());
 }
 for(auto f=m.faces_begin();f!=m.faces_end();++f)
  for(auto h=m.fh_iter(*f);h.is_valid();++h){auto p=m.point(m.to_vertex_handle(*h));m.set_texcoord2D(*h,Mesh::TexCoord2D(p[0]+10*f->idx(),p[1]));}
 assert(m.n_vertices()==9 && m.n_faces()==8 && m.n_edges()==16);
 int boundary=0,interior=0;
 for(auto e=m.edges_begin();e!=m.edges_end();++e)(m.is_boundary(*e)?boundary:interior)++;
 assert(boundary==8 && interior==8);
 auto h=m.find_halfedge(v[1],v[4]);assert(h.is_valid()&&!m.is_boundary(m.edge_handle(h)));
 auto op=m.opposite_halfedge_handle(h);
 assert(m.texcoord2D(h)[0]!=m.texcoord2D(m.prev_halfedge_handle(op))[0]);
 int incident=0;for(auto f=m.vf_iter(v[4]);f.is_valid();++f)++incident;
 assert(incident==6);
 assert(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
 std::cout<<"OPENMESH_BASELINE_OK V=9 E=16 F=8 boundary=8 center-fan=6 corner-seam=distinct\n";
}
