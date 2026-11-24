#include <opensubdiv/bfr/clipSurface.h>
#include "grid.h"
#include <cmath>
#include <string>
int main() {
    Grid g; OpenSubdiv::Bfr::ClippedMesh m;std::string error;
    OpenSubdiv::Bfr::ClipPlane p{{0,0,1},1};OpenSubdiv::Bfr::ClipOptions o{1e-3,1e-8,16};
    if(!OpenSubdiv::Bfr::ClipRegularSurfaceMesh(*g.topology,g.positions,g.uv,{5,6,9,10},p,o,m,error))return 1;
    if(m.positions.empty()||m.triangles.empty()||m.corners.size()!=3*m.triangles.size()||m.sourceFaces.size()!=m.triangles.size())return 2;
    double area=0;
    for(const auto &t:m.triangles) {
        for(auto i:t)if(i>=m.positions.size())return 3;
        auto a=m.positions[t[0]],b=m.positions[t[1]],c=m.positions[t[2]];
        area+=0.5*((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]));
    }
    return std::abs(area-4.0)<1e-8?0:4;
}
