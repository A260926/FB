#include <iostream>
#include <OpenGP/SurfaceMesh/SurfaceMesh.h>
#include <OpenGP/SurfaceMesh/IO/IO.h>
#include <cmath>
#include <cstdlib>
using namespace OpenGP;
int main(int argc,char** argv) {
    if(argc==2) {
        SurfaceMesh restored;
        if(!read_off(restored,argv[1]) || !restored.n_faces()) return 10;
        for(auto f:restored.faces()) if(restored.valence(f)!=3) return 11;
        std::cout<<"EXAMPLE_OFF triangles="<<restored.n_faces()<<"\n";
        return 0;
    }
    SurfaceMesh mesh;
    auto a=mesh.add_vertex(Vec3(0,0,0)), b=mesh.add_vertex(Vec3(2,0,0));
    auto c=mesh.add_vertex(Vec3(2,2,0)), d=mesh.add_vertex(Vec3(0,2,0));
    auto face=mesh.add_quad(a,b,c,d);
    if(!face.is_valid()) return 1;
    mesh.triangulate();
    if(mesh.n_faces()!=2 || mesh.n_edges()!=5) return 2;
    auto uv=mesh.halfedge_property<Vec3>("h:texcoord");
    auto mat=mesh.face_property<int>("f:material",7);
    int corners=0, boundary=0;
    for(auto f:mesh.faces()) {
        if(mat[f]!=7) return 3;
        for(auto h:mesh.halfedges(f)) {
            auto p=mesh.position(mesh.to_vertex(h)); uv[h]=Vec3(p[0],p[1],1); ++corners;
            if(mesh.is_boundary(mesh.opposite_halfedge(h))) ++boundary;
        }
    }
    mesh.update_face_normals(); mesh.update_vertex_normals();
    if(corners!=6 || boundary!=4) return 4;
    if(!write_off(mesh,".task-build/baseline.off")) return 5;
    SurfaceMesh restored;
    if(!read_off(restored,".task-build/baseline.off") || restored.n_faces()!=2 || restored.n_vertices()!=4) return 6;
    restored.garbage_collection();
    std::cout<<"BASELINE triangulate=2 corners=6 boundary=4 material=7 off_roundtrip=4/2\n";
}
