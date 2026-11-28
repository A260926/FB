#include <iostream>
#include <stdexcept>
#include "grid.hh"
#include <OpenVolumeMesh/Unstable/Topology/SheetCollapse.hh>
#include <cmath>
using namespace OpenVolumeMesh;
int main() {
    GeometricHexahedralMeshV3d m;task_grid(m,3);
    auto he=m.find_halfedge(VertexHandle(4),VertexHandle(8));
    if(!he.is_valid()) return 30;
    SheetCollapseResult result;
    if(!collapse_hex_sheet(m,m.edge_handle(he),result,1e-12,10)) return 31;
    if(m.n_logical_cells()!=2 || m.n_logical_vertices()!=12 || task_internal_faces(m)!=1) return 32;
    if(result.removed_cells.size()!=1 || result.removed_cells[0]!=CellHandle(1) || result.vertex_map.size()!=8) return 33;
    for(int i=0;i<8;++i) {
        const auto& p=result.vertex_map[i];
        if(p.first!=VertexHandle(i+4) || p.second!=VertexHandle(4+i%4)) return 34;
        if(std::abs(m.vertex(p.second)[2]-1.5)>1e-12) return 35;
    }
    for(auto c:m.cells()) {
        unsigned hfs=0;for(auto hf:m.cell(c).halffaces()) {if(!m.incident_cell(hf).is_valid())return 36;++hfs;}
        if(hfs!=6) return 37;
    }
    std::cout<<"API_CONSUMER collapse=ok cells=2 shared_faces=1 midpoint_z=1.5\n";
}
