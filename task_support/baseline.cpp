#include <iostream>
#include <stdexcept>
#include "grid.hh"
#include <OpenVolumeMesh/FileManager/FileManager.hh>
using namespace OpenVolumeMesh;
int main(int argc,char** argv) {
    IO::FileManager io;
    if(argc==2) {
        GeometricHexahedralMeshV3d m;
        if(!io.readFile(argv[1],m,true,true) || m.n_logical_cells()!=2 || task_internal_faces(m)!=1) return 20;
        std::cout<<"EXAMPLE_OVM cells=2 internal_faces=1\n";return 0;
    }
    GeometricHexahedralMeshV3d m;task_grid(m,3);
    if(m.n_vertices()!=16 || m.n_faces()!=16 || m.n_cells()!=3 || task_internal_faces(m)!=2) return 1;
    auto uv=m.request_halfface_property<std::vector<double>>("hf:uv");
    auto material=m.request_cell_property<int>("material",3);
    m.set_persistent(uv,true);m.set_persistent(material,true);
    unsigned corners=0;
    for(auto hf:m.halffaces()) {
        std::vector<double> v;
        for(auto he:m.halfface_halfedges(hf)) {
            auto p=m.vertex(m.halfedge(he).from_vertex());v.push_back(p[0]+hf.idx());v.push_back(p[1]);++corners;
        }
        uv[hf]=v;
    }
    if(corners!=128 || !io.writeFile(".task-build/baseline.ovm",m)) return 2;
    GeometricHexahedralMeshV3d read;
    if(!io.readFile(".task-build/baseline.ovm",read,true,true) || read.n_cells()!=3 || task_internal_faces(read)!=2) return 3;
    auto uv2=read.request_halfface_property<std::vector<double>>("hf:uv");
    auto mat2=read.request_cell_property<int>("material");
    for(auto hf:m.halffaces()) if(uv[hf]!=uv2[hf]) return 4;
    for(auto c:read.cells()) if(mat2[c]!=3) return 5;
    std::cout<<"BASELINE hex_cells=3 shared_faces=2 corner_values=256 uv_roundtrip=ok material=3\n";
}
