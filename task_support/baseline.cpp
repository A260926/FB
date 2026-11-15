#include <OpenVolumeMesh/Mesh/TetrahedralMesh.hh>
#include <OpenVolumeMesh/Mesh/PolyhedralMesh.hh>
#include <OpenVolumeMesh/FileManager/FileManager.hh>
#include <iostream>
#include <stdexcept>
using namespace OpenVolumeMesh;
static void require(bool x){if(!x)throw std::runtime_error("baseline failed");}
int main(){
 GeometricTetrahedralMeshV3d t;
 auto a=t.add_vertex(Vec3d(0,0,0)),b=t.add_vertex(Vec3d(1,0,0)),c=t.add_vertex(Vec3d(0,1,0));
 auto d=t.add_vertex(Vec3d(0,0,1)),e=t.add_vertex(Vec3d(0,0,-1));
 auto c0=t.add_cell(a,b,c,d,true); auto c1=t.add_cell(a,c,b,e,true);
 require(c0.is_valid()&&c1.is_valid());
 GeometricPolyhedralMeshV3d m; m=t; m.enable_deferred_deletion(true);
 auto material=m.request_cell_property<int>("c:material",0);material[c0]=7;material[c1]=9;m.set_persistent(material);
 auto uv=m.request_halfface_property<std::vector<double>>("hf:uv",{});
 auto normal=m.request_halfface_property<std::vector<double>>("hf:normal",{});
 for(auto h:m.halffaces()){uv[h]={double(h.idx()),0,1,0,0,1};normal[h]={0,0,1,0,0,1,0,0,1};}
 m.set_persistent(uv);m.set_persistent(normal);
 int shared=0;for(auto f:m.faces()){if(m.incident_cell(m.halfface_handle(f,0)).is_valid()&&m.incident_cell(m.halfface_handle(f,1)).is_valid())++shared;}
 require(shared==1&&m.n_vertices()==5&&m.n_cells()==2);
 IO::FileManager io;require(io.writeFile(".task-build/baseline.ovm",m));
 GeometricPolyhedralMeshV3d n;require(io.readFile(".task-build/baseline.ovm",n));
 auto uv2=n.request_halfface_property<std::vector<double>>("hf:uv",{});
 auto mat2=n.request_cell_property<int>("c:material",0);
 require(n.n_cells()==2&&n.n_faces()==7&&mat2[CellHandle(0)]==7&&mat2[CellHandle(1)]==9);
 double sum=0;for(auto h:n.halffaces()){require(uv2[h]==uv[h]);sum+=uv2[h][0];}
 std::cout<<"BASELINE shared="<<shared<<" cells="<<n.n_cells()<<" uv_checksum="<<sum<<" roundtrip=1\n";
}
