#include <array>
#include <cmath>
#include <iostream>
#include "task_support/mesh.hpp"
#include <vcg/complex/algorithms/harmonic.h>
int main(){
 TMesh obtuse;
 task_mesh(obtuse,{{0,0,0},{2,0,0},{.1,.01,0}},{{0,1,2}});
 const double w=vcg::tri::Harmonic<TMesh>::CotangentWeight<double>(obtuse.face[0],0);
 if(!std::isfinite(w)) return 1;
 std::cout<<"DEFAULT_BOUNDARY_WEIGHT="<<w<<"\n";
 TMesh fan;
 task_mesh(fan,{{0,0,0},{1,0,0},{.3,1,0},{.3,0,1},{.3,-1,0}},{{0,1,2},{0,1,3},{0,1,4}});
 auto* f=&fan.face[0];int edge=0;int n=0;
 do {int e=f->FFi(edge);f=f->FFp(edge);edge=e;++n;if(n>3)return 2;}while(f!=&fan.face[0]);
 if(n!=3)return 3;
 std::cout<<"NONMANIFOLD_FF_CYCLE="<<n<<"\n";
 TMesh disk;
 task_mesh(disk,{{0,0,0},{1,0,0},{1,1,0},{0,1,0},{.3,.4,0}},{{0,1,4},{1,2,4},{2,3,4},{3,0,4}});
 using H=vcg::tri::Harmonic<TMesh,double>;
 H::ConstraintVec constraints;
 for(int i=0;i<4;++i)constraints.push_back({&disk.vert[i],disk.vert[i].P()[0]});
 auto values=vcg::tri::Allocator<TMesh>::GetPerVertexAttribute<double>(disk,"field");
 if(!H::ComputeScalarField(disk,constraints,values))return 4;
 if(!std::isfinite(values[4])||std::abs(values[4]-.3)>1e-6)return 5;
 std::cout<<"DEFAULT_HARMONIC_CENTER="<<values[4]<<"\nVCG_TUFTED_BASELINE_OK\n";
}
