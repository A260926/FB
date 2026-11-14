#include <manifold/manifold.h>
#include <cstdio>
#include <cmath>
using namespace manifold;
int main(){
 Manifold shell=Manifold::Cube({2,3,4}).Refine(2)-Manifold::Cube({1,1,6}).Translate({.5,1,-1});
 int scalarCalls=0,batchCalls=0;
 auto scalar=shell.Warp([&](vec3& v){++scalarCalls;v.x+=.125*v.z*v.z;});
 std::printf("scalar_callbacks_before_observation=%d\n",scalarCalls);
 auto batch=shell.WarpBatch([&](VecView<vec3> vv){++batchCalls;for(auto& v:vv)v.x+=.125*v.z*v.z;});
 std::printf("batch_callbacks_before_observation=%d\n",batchCalls);
 if(scalar.Status()!=Manifold::Error::NoError||batch.Status()!=Manifold::Error::NoError)return 1;
 double delta=std::abs(scalar.Volume()-batch.Volume());
 if(delta>1e-10||scalar.NumVert()==0||scalar.Volume()<=0)return 2;
 if(scalarCalls!=int(scalar.NumVert())||batchCalls!=1)return 3;
 auto original=shell.GetMeshGL64(),warped=scalar.GetMeshGL64();
 if(original.runOriginalID!=warped.runOriginalID)return 4;
 double before=shell.Volume();
 auto transformed=scalar.Translate({1,2,3}).Scale({2,1,1});
 if(std::abs(transformed.Volume()-2*scalar.Volume())>1e-9)return 5;
 if(std::abs(shell.Volume()-before)>1e-10)return 6;
 std::printf("vertices=%zu triangles=%zu scalar_batch_volume_delta=%.3g\n",scalar.NumVert(),scalar.NumTri(),delta);
 std::printf("source_volume=%.9g warped_volume=%.9g affine_volume=%.9g\n",before,scalar.Volume(),transformed.Volume());
 std::puts("BASELINE_MANIFOLD_WARP_OK");
}
