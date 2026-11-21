#include <libnurbs/Surface/Surface.hpp>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace libnurbs;
static void need(bool x) { if(!x) throw std::runtime_error("baseline assertion"); }
int main() {
  Surface c; c.DegreeU=2;c.DegreeV=1;
  c.KnotsU=KnotVector{{0,0,0,1,1,1}};c.KnotsV=KnotVector{{0,0,1,1}};
  c.ControlPoints=ControlPointGrid(3,2);
  for(int j=0;j<2;++j){c.ControlPoints.Get(0,j)=Vec4(1,0,j,1);c.ControlPoints.Get(1,j)=Vec4(1,1,j,std::sqrt(.5));c.ControlPoints.Get(2,j)=Vec4(0,1,j,1);}
  auto p=c.Evaluate(.5,.5);need((p-Vec3(std::sqrt(.5),std::sqrt(.5),.5)).norm()<1e-12);
  auto du=c.EvaluateDerivative(.37,.61,1,0),dv=c.EvaluateDerivative(.37,.61,0,1);
  Vec3 n=du.cross(dv).normalized();need(std::abs(n.norm()-1)<1e-12);
  auto refined=c.InsertKnotU(.37).InsertKnotV(.61);double delta=0;
  for(int i=0;i<=8;++i)for(int j=0;j<=8;++j)delta=std::max(delta,(refined.Evaluate(i/8.,j/8.)-c.Evaluate(i/8.,j/8.)).norm());
  need(delta<1e-11);
  double rt=0;for(bool binary:{false,true}){std::stringstream ss;c.SaveToFile(ss,binary);Surface restored;restored.LoadFromFile(ss);rt=std::max(rt,(restored.Evaluate(.37,.61)-c.Evaluate(.37,.61)).norm());}need(rt<1e-11);
  Surface s;s.DegreeU=s.DegreeV=2;s.KnotsU=s.KnotsV=KnotVector{{0,0,0,1,1,1}};s.ControlPoints=ControlPointGrid(3,3);
  for(int j=0;j<3;++j)for(int i=0;i<3;++i)s.ControlPoints.Get(i,j)=Vec4(i/2.,j/2.,.2*i*(i-1)/2.+.15*(i/2.)*(j/2.)+.1*j*(j-1)/2.,1);
  auto jet=s.EvaluateAll(.37,.61,2,2);need(std::abs(jet.Get(2,0).z()-.4)<1e-12);need(std::abs(jet.Get(1,1).z()-.15)<1e-12);
  Surface naive=s;const double d=.12;
  for(int j=0;j<3;++j)for(int i=0;i<3;++i){Vec3 a=s.EvaluateDerivative(i/2.,j/2.,1,0),b=s.EvaluateDerivative(i/2.,j/2.,0,1);naive.ControlPoints.Get(i,j).head<3>()+=d*a.cross(b).normalized();}
  Vec3 a=s.EvaluateDerivative(.37,.61,1,0),b=s.EvaluateDerivative(.37,.61,0,1);
  double gap=(naive.Evaluate(.37,.61)-s.Evaluate(.37,.61)-d*a.cross(b).normalized()).norm();
  std::cout<<"SURFACE_BASELINE cylinder="<<p.transpose()<<" insert_error="<<delta<<" roundtrip="<<rt<<" naive_offset_gap="<<gap<<"\nBASELINE_COMPLETE\n";
}
