#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
 typedef OpenMesh::TriMesh_ArrayKernelT<> Mesh;Mesh m;
 auto a=m.add_vertex(Mesh::Point(0,0,0));auto b=m.add_vertex(Mesh::Point(2,0,0));
 auto c=m.add_vertex(Mesh::Point(2,1,0));auto d=m.add_vertex(Mesh::Point(0,2,1));
 assert(m.add_face(a,b,c).is_valid());assert(m.add_face(a,c,d).is_valid());
 auto h=m.find_halfedge(a,c);assert(h.is_valid());auto e=m.edge_handle(h);
 assert(!m.is_boundary(e));assert(m.is_flip_ok(e));
 double oldLength=(m.point(a)-m.point(c)).norm();m.flip(e);
 assert(m.find_halfedge(b,d).is_valid()||m.find_halfedge(d,b).is_valid());
 assert(m.n_vertices()==4&&m.n_edges()==5&&m.n_faces()==2);
 assert(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
 std::cout<<"BASELINE OpenMesh flip PASS vertices=4 edges=5 faces=2 old_length="<<oldLength<<"\n";
}
