#include "fixtures.h"
#include <iostream>
#include <cmath>
int main() {
  auto c=fixture::kink();
  auto [normal,n]=c.InsertKnot(.25).RemoveKnot(.25,1,1e-10);
  double good=(normal.Evaluate(.23)-c.Evaluate(.23)).norm();
  if(n!=1 || good>1e-9) return 1;
  std::cout<<"NORMAL_INSERT_REMOVE_OK removed="<<n<<" error="<<good<<"\n";
  auto [rejected,z]=c.RemoveKnot(.5,1,1e-10);
  double delta=(rejected.Evaluate(.23)-c.Evaluate(.23)).norm();
  std::cout<<"REFUSED removed="<<z<<" geometry_delta="<<delta<<"\n";
  auto inserted=c.InsertKnot(.5);
  auto [part,t]=inserted.RemoveKnot(.5,2,1e-10);
  std::cout<<"PARTIAL removed="<<t<<" controls="<<part.ControlPoints.size()<<" expected_controls="<<inserted.ControlPoints.size()-t<<" knots="<<part.Knots.Values().size()<<"\n";
  auto s=fixture::mixed();auto [out,common]=s.RemoveKnotU(.5,2,1e-10);
  std::cout<<"SURFACE_COMMON removed="<<common<<" u_count="<<out.ControlPoints.UCount<<"\n";
  return 0;
}
