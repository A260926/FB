#include "NurbsSurface.h"
#include "LNObject.h"
#include <cmath>
#include <iostream>
int main(){
 using namespace LNLib;
 LN_NurbsSurface s;s.DegreeU=2;s.DegreeV=1;
 s.KnotVectorU={0,0,0,.25,.25,.5,.5,.75,.75,1,1,1};s.KnotVectorV={0,0,1,1};
 double xy[9][2]={{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1},{1,0}};
 for(int i=0;i<9;++i){double w=i%2?std::sqrt(.5):1;s.ControlPoints.push_back({XYZW(XYZ(xy[i][0],xy[i][1],0),w),XYZW(XYZ(xy[i][0],xy[i][1],2),w)});}
 auto p=NurbsSurface::GetPointOnSurface(s,UV(.125,.3));
 if(std::abs(p.X()-std::sqrt(.5))>1e-10 || std::abs(p.Y()-std::sqrt(.5))>1e-10 || std::abs(p.Z()-.6)>1e-10)return 1;
 auto mesh=NurbsSurface::Triangulate(s,9,3,false);
 if(mesh.Vertices.size()!=27 || mesh.Faces.size()!=32 || mesh.UVs.size()!=27)return 2;
 unsigned paired=0;for(int j=0;j<3;++j){if(mesh.Vertices[j].Distance(mesh.Vertices[24+j])>1e-10)return 3;++paired;}
 for(auto const& f:mesh.Faces){if(f.size()!=3)return 4;for(int v:f)if(v<0 || v>=27)return 5;}
 std::cout<<"CYLINDER point="<<p.X()<<","<<p.Y()<<","<<p.Z()<<" vertices="<<mesh.Vertices.size()<<" triangles="<<mesh.Faces.size()<<" seam_pairs="<<paired<<"\n";
 std::cout<<"PRODUCTION_BASELINE_OK\n";
}
