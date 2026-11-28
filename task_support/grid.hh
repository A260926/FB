#pragma once
#include <OpenVolumeMesh/Mesh/HexahedralMesh.hh>
#include <vector>
// Deterministic input fixture, following upstream add_cell's documented order.
inline void task_grid(OpenVolumeMesh::GeometricHexahedralMeshV3d& m,int layers,double height=1) {
    using namespace OpenVolumeMesh;
    m.enable_bottom_up_incidences(true);m.enable_deferred_deletion(true);
    for(int z=0;z<=layers;++z) {
        m.add_vertex(Geometry::Vec3d(0,0,z*height));m.add_vertex(Geometry::Vec3d(1,0,z*height));
        m.add_vertex(Geometry::Vec3d(1,1,z*height));m.add_vertex(Geometry::Vec3d(0,1,z*height));
    }
    for(int z=0;z<layers;++z) {
        int b=z*4,t=b+4;
        std::vector<VertexHandle> v={VertexHandle(b),VertexHandle(b+1),VertexHandle(b+2),VertexHandle(b+3),VertexHandle(t),VertexHandle(t+3),VertexHandle(t+2),VertexHandle(t+1)};
        if(!m.add_cell(v,true).is_valid()) throw std::runtime_error("grid fixture construction");
    }
}
inline unsigned task_internal_faces(const OpenVolumeMesh::GeometricHexahedralMeshV3d& m) {
    unsigned n=0;
    for(auto f:m.faces()) if(!m.is_deleted(f) && m.incident_cell(m.halfface_handle(f,0)).is_valid() && m.incident_cell(m.halfface_handle(f,1)).is_valid()) ++n;
    return n;
}
