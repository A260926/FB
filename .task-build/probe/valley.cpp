#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
#include <math.h>
int main(){
  // V-channel: y in [0,1], z = -depth*|y-0.5|*2 (a V with opening upward). A collapse that bridges the fold
  // would create a lid high above the valley floor, far from the walls.
  for(unsigned n=6;n<=12;n+=3) for(float mse=0.01f;mse<=0.1f;mse+=0.03f) for(unsigned d=4; d<=16; d+=4){
    std::vector<float> pos,attr; std::vector<unsigned> idx;
    for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n;float y=float(j)/n;float z=-2.f*fabsf(y-0.5f);pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
    for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
    std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
    size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,0.5f,0,&err,mse,d,&st);
    if(st.rejected_distance>0) printf("HIT n=%u mse=%.2f d=%u out=%zu acc=%zu rejD=%zu rejB=%zu cert=%g\n",n,mse,d,c,st.accepted_collapses,st.rejected_distance,st.rejected_budget,st.certified_error);
  }
  puts("done");
  return 0;
}
