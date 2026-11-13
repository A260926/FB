#include "NurbsSurface.h"
#include "XYZW.h"
#include "UV.h"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <stdexcept>
#include <algorithm>
using namespace LNLib;
void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
LN_NurbsSurface left_surface() {
  LN_NurbsSurface s;
  s.DegreeU=2; s.DegreeV=1;
  s.KnotVectorU={0,0,0,1,1,1}; s.KnotVectorV={0,0,1,1};
  for(int i=0;i<3;++i) {
    std::vector<XYZW> row;
    for(int j=0;j<2;++j) row.emplace_back(XYZ(.5*i,j,(i==1)? .4*j:0),1);
    s.ControlPoints.push_back(row);
  }
  return s;
}
LN_NurbsSurface right_surface() {
  LN_NurbsSurface s;
  s.DegreeU=2; s.DegreeV=2;
  s.KnotVectorU={0,0,0,1,1,1}; s.KnotVectorV={0,0,0,1,1,1};
  const double w[]={1,1.5,2}, y[]={0,.5,2};
  for(int i=0;i<3;++i) {
    std::vector<XYZW> row;
    for(int j=0;j<3;++j) {
      double z=(i==0)?0:(.1*i*(2-j)+.07*i*j*j);
      row.emplace_back((1+.5*i)*w[j],y[j],z*w[j],w[j]);
    }
    s.ControlPoints.push_back(row);
  }
  return s;
}
double error(const LN_NurbsSurface& a,const LN_NurbsSurface& b) {
  double maximum=0;
  for(int i=0;i<=16;++i) for(int j=0;j<=16;++j) {
    UV uv(i/16.,j/16.);
    maximum=std::max(maximum,NurbsSurface::GetPointOnSurface(a,uv).Distance(NurbsSurface::GetPointOnSurface(b,uv)));
  }
  return maximum;
}
int main() {
  try {
    auto a=left_surface(), b=right_surface();
    NurbsSurface::Check(a); NurbsSurface::Check(b);
    double seam=0;
    for(int k=0;k<=32;++k) {
      double v=k/32.;
      auto x=NurbsSurface::GetPointOnSurface(a,UV(1,v));
      auto y=NurbsSurface::GetPointOnSurface(b,UV(0,v));
      seam=std::max(seam,x.Distance(y));
      check(y.Distance(XYZ(1,v,0))<1e-12,"analytic seam reference");
    }
    check(seam<1e-12,"source seam must agree geometrically");
    LN_NurbsSurface av, au, ar, br;
    NurbsSurface::ElevateDegree(a,1,false,av);
    check(std::abs(av.ControlPoints.back()[0].GetW()-1)<1e-12,"left denominator reference");
    check(std::abs(b.ControlPoints.front()[2].GetW()/av.ControlPoints.back()[2].GetW()-2)<1e-12,"nonconstant seam gauge witness");
    check(std::abs(b.ControlPoints.front()[0].GetW()/av.ControlPoints.back()[0].GetW()-1)<1e-12,"nonconstant seam gauge witness start");
    NurbsSurface::ElevateDegree(av,1,true,au);
    NurbsSurface::RefineKnotVector(au,{.25,.75},false,ar);
    NurbsSurface::InsertKnot(b,.375,1,true,br);
    double delta=std::max({error(a,av),error(a,au),error(a,ar),error(b,br)});
    check(delta<1e-11,"primitive operations must preserve full surfaces");
    auto derivatives=NurbsSurface::ComputeRationalSurfaceDerivatives(b,1,UV(.31,.43));
    check(std::isfinite(derivatives[1][0].GetX()),"original derivative evaluator");
    check(ar.ControlPoints.size()==4 && ar.ControlPoints[0].size()==5,"tensor layout and knot refinement");
    std::cout<<std::setprecision(17)<<"BASELINE original_library=PASS\n"
      <<"seam_same_geometry=PASS samples=33 max_error="<<seam<<'\n'
      <<"nonconstant_weight_ratio=PASS endpoints=1,2\n"
      <<"elevation_refinement_preserve=PASS samples=1156 max_error="<<delta<<'\n'
      <<"rational_derivative=PASS\n"
      <<"capability=primitive_operations_only\n";
    return 0;
  } catch(const std::exception& e) { std::cerr<<"baseline failure: "<<e.what()<<'\n'; return 1; }
}
