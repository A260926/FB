#include "NurbsSurface.h"
#include "XYZW.h"
#include "UV.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <exception>
using namespace LNLib;
int main(){try{
 LN_NurbsSurface s;s.DegreeU=2;s.DegreeV=3;
 s.KnotVectorU={0,0,0,.3,.7,1,1,1};s.KnotVectorV={0,0,0,0,1,1,1,1};
 for(int i=0;i<5;++i){std::vector<XYZW> row;for(int j=0;j<4;++j){double w=.75+.1*(i+2*j);row.emplace_back(XYZ(.3*i,.4*j,.07*i*i+.11*i*j-.03*j*j),w);}s.ControlPoints.push_back(row);}
 NurbsSurface::Check(s);LN_NurbsSurface a,b;
 NurbsSurface::RefineKnotVector(s,{.15,.5,.85},true,a);NurbsSurface::RefineKnotVector(a,{.5},false,b);
 double error=0,derror=0;
 for(int i=0;i<11;++i)for(int j=0;j<9;++j){UV uv((i+.19)/11,(j+.23)/9);auto x=NurbsSurface::ComputeRationalSurfaceDerivatives(s,1,uv);auto y=NurbsSurface::ComputeRationalSurfaceDerivatives(b,1,uv);error=std::max(error,x[0][0].Distance(y[0][0]));derror=std::max({derror,x[1][0].Distance(y[1][0]),x[0][1].Distance(y[0][1])});}
 if(error>1e-10||derror>1e-9)return 2;
 auto mesh=NurbsSurface::Triangulate(s,7,6,false);
 if(mesh.Vertices.size()!=42||mesh.Faces.size()!=60||mesh.UVs.size()!=42||mesh.UVIndices.size()!=180)return 3;
 for(size_t i=0;i<mesh.Vertices.size();++i)if(mesh.Vertices[i].Distance(NurbsSurface::GetPointOnSurface(s,mesh.UVs[i]))>1e-12)return 4;
 if(b.ControlPoints.size()!=8||b.ControlPoints[0].size()!=5)return 5;
 std::printf("BASELINE tensor_refinement_exact points=99 max_error=%.3g derivative_error=%.3g\n",error,derror);
 std::printf("BASELINE original_grid vertices=%zu triangles=%zu uv_indices=%zu\n",mesh.Vertices.size(),mesh.Faces.size(),mesh.UVIndices.size());
 std::puts("BASELINE production_only no_hierarchy_implementation");return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"baseline exception: %s\n",e.what());return 1;}}
