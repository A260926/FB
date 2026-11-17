#include "BSplineLib/Splines/b_spline.hpp"
#include <iostream>
#include <cmath>
using namespace bsplinelib;
int main(){
 using P=parameter_spaces::ParameterSpace<2>;
 using V=vector_spaces::VectorSpace;
 auto u=std::make_shared<parameter_spaces::KnotVector>(std::vector<double>{0,0,1,1});
 auto v=std::make_shared<parameter_spaces::KnotVector>(std::vector<double>{0,0,1,1});
 auto p=std::make_shared<P>(P::KnotVectors_{u,v},P::Degrees_{1,1});
 V::Coordinates_ ca(4,3),cb(4,3);
 for(int i=0;i<4;++i){ca(i,0)=i%2;ca(i,1)=i/2;ca(i,2)=0.5*(i%2)*(i/2);cb(i,0)=2*(i%2);cb(i,1)=i/2;cb(i,2)=3;}
 auto a=std::make_shared<V>(std::move(ca)); auto b=std::make_shared<V>(std::move(cb));
 splines::BSpline<2> first(p,a),peer(p,b);
 double uv[2]={0.25,0.75}; auto before=first(uv);auto other_before=peer(uv);
 if(std::abs(before[0]-.25)>1e-12||std::abs(other_before[0]-.5)>1e-12)return 1;
 first.InsertKnot(0,.5);
 auto after=first(uv);double err=0;for(int i=0;i<3;++i)err+=std::abs(after[i]-before[i]);
 std::cout<<"NORMAL_POINT "<<before[0]<<" "<<before[1]<<" "<<before[2]<<"\n";
 std::cout<<"SHARED_BASIS basis="<<p->GetTotalNumberOfBasisFunctions()<<" edited="<<a->GetNumberOfCoordinates()<<" peer="<<b->GetNumberOfCoordinates()<<"\n";
 std::cout<<"EDITED_SHAPE_ERROR "<<err<<"\n";
 if(p->GetTotalNumberOfBasisFunctions()!=6||a->GetNumberOfCoordinates()!=6||b->GetNumberOfCoordinates()!=4||err>1e-12)return 2;
 std::cout<<"BSPLINELIB_BASELINE_OK\n";
}
