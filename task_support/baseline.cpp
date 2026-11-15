#include <OpenGP/SurfaceMesh/SurfaceMesh.h>
#include <iostream>
#include <stdexcept>
using namespace OpenGP;
static void require(bool b){if(!b)throw std::runtime_error("baseline failed");}
int main(){
 SurfaceMesh m;
 auto a=m.add_vertex(Vec3(0,0,0)),b=m.add_vertex(Vec3(1,0,0)),c=m.add_vertex(Vec3(1,1,0)),d=m.add_vertex(Vec3(0,1,0));
 auto f=m.add_quad(a,b,c,d);require(f.is_valid());m.triangulate();require(m.n_faces()==2&&m.n_vertices()==4);
 bool flipped=false;for(auto e:m.edges()){if(!m.is_boundary(e)&&m.is_flip_ok(e)){m.flip(e);flipped=true;break;}}
 require(flipped);
 auto uv=m.halfedge_property<Vec3>("h:texcoord",Vec3(0,0,0));for(auto h:m.halfedges())uv[h]=Vec3(.25f,.5f,1);
 auto vn=m.vertex_property<Vec3>("v:normal",Vec3(0,0,1));for(auto v:m.vertices())vn[v]=Vec3(0,0,1);
 require(m.write(".task-build/baseline.obj"));SurfaceMesh n;require(n.read(".task-build/baseline.obj"));
 require(n.n_faces()==2&&n.n_vertices()==4&&n.is_triangle_mesh());
 auto uv2=n.get_halfedge_property<Vec3>("h:texcoord");require(bool(uv2));int corners=0;
 for(auto f2:n.faces()){auto h=n.halfedges(f2),end=h;do{require((uv2[*h]-Vec3(.25f,.5f,1)).norm()<1e-6f);++corners;}while(++h!=end);}
 require(corners==6);std::cout<<"BASELINE vertices="<<n.n_vertices()<<" triangles="<<n.n_faces()<<" flipped="<<flipped<<" uv_corners="<<corners<<" obj_roundtrip=1\n";
}
