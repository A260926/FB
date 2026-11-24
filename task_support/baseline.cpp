#include "grid.h"
#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <opensubdiv/bfr/tessellation.h>
#include <cmath>
#include <cstdio>
int main() {
    Grid g;if(!g.topology)return 1;
    OpenSubdiv::Bfr::RefinerSurfaceFactory<> factory(*g.topology);
    OpenSubdiv::Bfr::Surface<double> s,t;
    if(!factory.InitVertexSurface(5,&s)||!s.IsRegular()||!factory.InitFaceVaryingSurface(5,&t,0))return 2;
    std::vector<double> pp(s.GetNumPatchPoints()*3),tp(t.GetNumPatchPoints()*2);
    s.PreparePatchPoints(g.positions.data(),3,pp.data(),3);t.PreparePatchPoints(g.uv.data(),2,tp.data(),2);
    double uv[2]={0.5,0.5},p[3],du[3],dv[3],tex[2];
    s.Evaluate(uv,pp.data(),3,p,du,dv);t.Evaluate(uv,tp.data(),2,tex);
    if(std::abs(p[0]-1.5)>1e-10||std::abs(p[1]-1.5)>1e-10||std::abs(p[2])>1e-10||!std::isfinite(tex[0]))return 3;
    OpenSubdiv::Bfr::Tessellation tess(s.GetParameterization(),4);
    if(tess.GetNumCoords()<9||tess.GetNumFacets()<4)return 4;
    std::printf("BFR_BASELINE regular=1 position=%.3f,%.3f,%.3f uv=%.3f,%.3f coords=%d facets=%d\n",p[0],p[1],p[2],tex[0],tex[1],tess.GetNumCoords(),tess.GetNumFacets());
}
