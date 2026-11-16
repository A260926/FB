#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
#include <math.h>
// A tall thin fin: strip of width w along x, height varying, y=0..1. Collapsing along the fin top can create
// triangles entirely inside the fin volume, far from the fin surface? test many.
int main(){
  for(unsigned n=4;n<=16;n+=4)for(float h=0.2f;h<=1.0f;h+=0.4f)for(float mse=0.001f;mse<=0.05f;mse*=5.f)for(unsigned d=0;d<=4;d+=2)for(float te : {0.5f,8.f}){
    std::vector<float> pos,attr; std::vector<unsigned> idx;
    for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=1;++i){float x=0.02f*i,y=float(j)/n,z=h*sinf(y*3.14159f);pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
    for(unsigned j=0;j<n;++j){unsigned a=j*2,b=a+1,c=a+2,dd=a+3;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
    std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
    size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,te,0,&err,mse,d,&st);
    if(st.rejected_distance>0) printf("HIT n=%u h=%.1f mse=%g d=%u te=%.1f out=%zu acc=%zu rejD=%zu rejB=%zu\n",n,h,mse,d,te,c,st.accepted_collapses,st.rejected_distance,st.rejected_budget);
  }
  puts("done");
  return 0;
}
