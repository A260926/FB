#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Subdivider/Uniform/LoopT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>
#include <iostream>
#include <cmath>
int loop_baseline(){typedef OpenMesh::TriMesh_ArrayKernelT<> Mesh;Mesh m;auto a=m.add_vertex(Mesh::Point(0,0,0));auto b=m.add_vertex(Mesh::Point(2,0,0));auto c=m.add_vertex(Mesh::Point(0,1,0));if(!m.add_face(a,b,c).is_valid())return 1;
 OpenMesh::Subdivider::Uniform::LoopT<Mesh> sub;sub.attach(m);bool good=sub(1);sub.detach();if(!good||m.n_vertices()!=6||m.n_faces()!=4||!OpenMesh::Utils::MeshCheckerT<Mesh>(m).check())return 2;
 double area=0;for(auto f=m.faces_begin();f!=m.faces_end();++f){auto it=m.cfv_iter(*f);auto x=m.point(*it);++it;auto y=m.point(*it);++it;auto z=m.point(*it);area+=((y-x)%(z-x)).norm()*.5;}if(!(area>0))return 3;
 std::cout<<"OPENMESH_BASELINE vertices=6 faces=4 checker=ok subdivider=Loop\n";return 0;}


struct DoubleTraits:OpenMesh::DefaultTraits { typedef OpenMesh::Vec3d Point; typedef OpenMesh::Vec3d Normal; };
int edit_baseline(){
 using Mesh=OpenMesh::TriMesh_ArrayKernelT<DoubleTraits>; Mesh m;
 auto a=m.add_vertex(Mesh::Point(0,0,0)); auto b=m.add_vertex(Mesh::Point(2,0,0));
 auto c=m.add_vertex(Mesh::Point(0,2,0)); auto d=m.add_vertex(Mesh::Point(0,0,2));
 if(!m.add_face(a,b,c).is_valid()||!m.add_face(b,a,d).is_valid())return 10;
 auto h=m.find_halfedge(a,b);auto v=m.split(m.edge_handle(h),Mesh::Point(.5,0,0));
 if(m.n_vertices()!=5||m.n_edges()!=8||m.n_faces()!=4||m.valence(v)!=4||!OpenMesh::Utils::MeshCheckerT<Mesh>(m).check())return 11;
 int n=0;for(auto e=m.edges_begin();e!=m.edges_end();++e)if(m.is_boundary(*e))++n;
 if(n!=4)return 12;
 double total=0;for(auto f=m.faces_begin();f!=m.faces_end();++f){auto it=m.cfv_iter(*f);auto p=m.point(*it);++it;auto q=m.point(*it);++it;auto r=m.point(*it);total+=((q-p)%(r-p)).norm()*.5;}
 if(std::fabs(total-4)>1e-12)return 13;
 std::cout<<"EDIT_BASELINE shared_edge_split vertices=5 edges=8 faces=4 boundary_edges=4 area=4 checker=ok\n";return 0;
}
int main(){int rc=loop_baseline();return rc?rc:edit_baseline();}
