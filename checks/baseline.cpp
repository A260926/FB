#include "NurbsCurve.h"
#include "NurbsSurface.h"
#include "LNObject.h"
#include "UV.h"
#include <cmath>
#include <iostream>
using namespace LNLib;
int main(){
 std::vector<LN_NurbsCurve> sections(3);
 for(int j=0;j<3;j++){
  sections[j].Degree=2; sections[j].KnotVector={0,0,0,1,1,1};
  sections[j].ControlPoints={XYZW(XYZ(0,0,j),1),XYZW(XYZ(1,1,j),1),XYZW(XYZ(2,0,j),1)};
 }
 LN_NurbsSurface surface;
 NurbsSurface::CreateLoftSurface(sections,surface,2,{0,.5,1});
 double e=0;
 for(int j=0;j<3;j++) for(int i=0;i<=8;i++){
  double u=i/8.;
  e=std::max(e,NurbsSurface::GetPointOnSurface(surface,UV(u,j*.5)).Distance(NurbsCurve::GetPointOnCurve(sections[j],u)));
 }
 if(!std::isfinite(e)||e>1e-9) return 1;
 std::cout<<"LNLIB_BASELINE_OK sections=3 max_error="<<e<<"\n";
 LN_NurbsSurface cylinder;
 cylinder.DegreeU=cylinder.DegreeV=2;
 cylinder.KnotVectorU={2,2,2,5,5,5}; cylinder.KnotVectorV={-3,-3,-3,1,1,1};
 const double xx[]={1,1,0},yy[]={0,1,1},ww[]={1,std::sqrt(.5),1};
 for(int i=0;i<3;i++){
  std::vector<XYZW> row;
  for(int j=0;j<3;j++)row.push_back(XYZW(XYZ(xx[i],yy[i],j*.5),ww[i]));
  cylinder.ControlPoints.push_back(row);
 }
 auto d=NurbsSurface::ComputeRationalSurfaceDerivatives(cylinder,2,UV(3.5,-1));
 auto point=d[0][0];
 double det=d[1][0].DotProduct(d[1][0])*d[0][1].DotProduct(d[0][1])-std::pow(d[1][0].DotProduct(d[0][1]),2);
 if(!std::isfinite(det)||det<=0||std::abs(point.GetX()*point.GetX()+point.GetY()*point.GetY()-1)>1e-12||std::abs(point.GetZ()-.5)>1e-12) return 2;
 std::cout<<"LNLIB_RATIONAL_DERIVATIVE_OK determinant="<<det<<"\n";
}
