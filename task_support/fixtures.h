#pragma once
#include <libnurbs/Curve/Curve.hpp>
#include <libnurbs/Surface/Surface.hpp>
namespace fixture {
inline libnurbs::Curve kink() {
  libnurbs::Curve c;
  c.Degree=2;c.Knots=libnurbs::KnotVector{{0,0,0,.5,1,1,1}};
  c.ControlPoints={{0,0,0,1},{1,1,0,2},{2,-.5,1,.7},{3,.2,0,1.2}};
  return c;
}
inline libnurbs::Curve bezier() {
  libnurbs::Curve c;c.Degree=2;c.Knots=libnurbs::KnotVector{{0,0,0,1,1,1}};
  c.ControlPoints={{0,0,0,1},{1,1,0,1},{3,0,0,1}};return c;
}
inline libnurbs::Surface mixed() {
  auto a=bezier().InsertKnot(.5,2);auto b=kink().InsertKnot(.5);
  libnurbs::Surface s;s.DegreeU=2;s.DegreeV=1;
  s.KnotsU=a.Knots;s.KnotsV=libnurbs::KnotVector{{0,0,1,1}};
  s.ControlPoints={5,2};
  for(int i=0;i<5;i++){s.ControlPoints.Get(i,0)=a.ControlPoints[i];auto p=b.ControlPoints[i];p.z()+=2;s.ControlPoints.Get(i,1)=p;}
  return s;
}
}
