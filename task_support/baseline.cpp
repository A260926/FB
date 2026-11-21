#include "BSplineLib/Splines/nurbs.hpp"
#include "BSplineLib/InputOutput/iges.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <algorithm>
int main(){
 using namespace bsplinelib;using P=parameter_spaces::ParameterSpace<1>;
 using V=vector_spaces::WeightedVectorSpace;using N=splines::Nurbs<1>;
 P::KnotVectors_ kv{std::make_shared<parameter_spaces::KnotVector>(parameter_spaces::KnotVector::Knots_{-2,-1,0,1,2,3,4,5,6})};
 auto p=std::make_shared<P>(kv,P::Degrees_{2});
 V::Coordinates_ xyz(6,3);V::Weights_ w(6);
 const double pos[4][3]={{1,0,0},{0,1,.3},{-1,0,-.2},{0,-1,.1}};
 for(int i=0;i<6;i++){for(int d=0;d<3;d++)xyz(i,d)=pos[i%4][d];w[i]=1+.1*(i%4);}
 auto v=std::make_shared<V>(xyz,w);auto curve=std::make_shared<N>(p,v);
 double before[9][3];for(int i=0;i<9;i++){double u=.1+.45*i;curve->Evaluate(&u,before[i]);}
 curve->InsertKnot(0,.5);
 double err=0;for(int i=0;i<9;i++){double u=.1+.45*i,out[3];curve->Evaluate(&u,out);for(int d=0;d<3;d++){if(!std::isfinite(out[d]))return 2;err=std::max(err,std::abs(out[d]-before[i][d]));}}
 const auto& c=v->GetCoordinates();const int count=v->GetNumberOfCoordinates(),m=count-2;double gap=0;
 for(int i=0;i<2;i++)for(int d=0;d<4;d++)gap=std::max(gap,std::abs(c(i,d)-c(m+i,d)));
 input_output::iges::Write(input_output::operations::Splines{curve},".task-build/baseline.igs",Precision{17});
 auto loaded=input_output::iges::Read(".task-build/baseline.igs");if(loaded.size()!=1)return 3;
 auto copy=std::dynamic_pointer_cast<N>(loaded[0]);if(!copy)return 4;
 double u=1.3,a[3],b[3];curve->Evaluate(&u,a);copy->Evaluate(&u,b);double io=0;for(int d=0;d<3;d++)io=std::max(io,std::abs(a[d]-b[d]));
 std::cout.precision(17);std::cout<<"BSPLINELIB_BASELINE samples=9 inserted_controls="<<count<<" shape_error="<<err<<" ordinary_padding_gap="<<gap<<" iges_error="<<io<<"\n";
 return err<1e-10&&io<1e-10?0:5;
}
