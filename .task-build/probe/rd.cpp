#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
int main(){
  for(unsigned n=6;n<=10;n+=2) for(float h=0.2f;h<=0.8f;h+=0.2f) for(float te=0.5f; te<=8.f; te*=4.f) for(unsigned d=8; d<=16; d+=4){
    std::vector<float> pos,attr; std::vector<unsigned> idx;
    for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n,z=0; if(i==n/2&&j==n/2)z=h; pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
    for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
    std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
    size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,te,0,&err,0.01f,d,&st);
    if(st.rejected_distance>0) printf("HIT n=%u h=%.1f te=%.1f d=%u out=%zu acc=%zu rejD=%zu rejB=%zu cert=%g\n",n,h,te,d,c,st.accepted_collapses,st.rejected_distance,st.rejected_budget,st.certified_error);
  }
  puts("done");
  return 0;
}
