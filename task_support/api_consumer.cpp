#include <iostream>
#include <OpenGP/SurfaceMesh/InsetRoof.h>
#include <cmath>
#include <vector>
using namespace OpenGP;
int main() {
    SurfaceMesh source;
    auto a=source.add_vertex(Vec3(0,0,0)),b=source.add_vertex(Vec3(2,0,0));
    auto c=source.add_vertex(Vec3(2,2,0)),d=source.add_vertex(Vec3(0,2,0));
    auto f=source.add_triangle(a,b,c),g=source.add_triangle(a,c,d);
    inset_roof(source,{f,g},Scalar(.25));
    SurfaceMesh& roof=source;
    if(!roof.n_faces()) return 31;
    double area=0,maxz=0; bool top=false;
    auto origins=roof.get_face_property<int>("f:source_face");
    auto generators=roof.get_face_property<int>("f:roof_edge");
    auto normals=roof.get_halfedge_property<Vec3>("h:normal");
    if(!origins || !generators || !normals) return 32;
    for(auto face:roof.faces()) {
        if(roof.valence(face)!=3 || (origins[face]!=f.idx() && origins[face]!=g.idx())) return 33;
        std::vector<Vec3> p;
        for(auto h:roof.halfedges(face)) {
            auto x=roof.position(roof.to_vertex(h)); p.push_back(x);
            if(!x.allFinite() || x[2]<-1e-5 || x[2]>.25001 || std::abs(normals[h].norm()-1)>1e-4) return 34;
            maxz=std::max(maxz,double(x[2]));
        }
        auto cross=(p[1]-p[0]).cross(p[2]-p[0]);
        if(cross[2]<=0) return 35;
        area+=cross[2]*.5;
        if(generators[face]==-1) {
            top=true;
            for(auto x:p) if(std::abs(x[2]-.25)>1e-5) return 36;
        }
    }
    if(std::abs(area-4)>1e-4 || std::abs(maxz-.25)>1e-5 || !top) return 37;
    std::cout<<"API_CONSUMER roof_projected_area=4 cap_height=0.25\n";
}
