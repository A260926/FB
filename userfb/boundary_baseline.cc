#include <OpenVolumeMesh/Mesh/TetrahedralMesh.hh>
#include <cassert>
#include <iostream>
using namespace OpenVolumeMesh;
int main() {
  GeometricTetrahedralMeshV3d m;
  auto a=m.add_vertex({0,0,0}), b=m.add_vertex({1,0,0});
  auto c=m.add_vertex({0,1,0}), d=m.add_vertex({0,0,1});
  auto e=m.add_vertex({0,0,-1});
  auto x=m.add_cell(a,b,c,d,true), y=m.add_cell(a,c,b,e,true);
  assert(x.is_valid() && y.is_valid());
  size_t global=0, local_global=0, local_faces=0;
  for(auto f:m.faces()) if(m.is_boundary(f)) ++global;
  for(auto h:m.cell(x).halffaces()) {
    ++local_faces;
    if(m.is_boundary(h.face_handle())) ++local_global;
  }
  assert(m.n_cells()==2 && m.n_faces()==7 && global==6);
  assert(local_faces==4 && local_global==3);
  std::cout<<"OVM two-cell shared-face topology PASS\n";
  std::cout<<"OVM global boundary 6, selected cell global faces 3, selected cell faces 4\n";
}
