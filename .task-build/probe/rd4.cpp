#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
int main(){
  for(unsigned n=8;n<=12;n+=4)for(float h=0.5f;h<=1.5f;h+=0.5f)for(float mse=0.005f;mse<=0.05f;mse*=3.f)for(unsigned d=6;d<=16;d+=5)for(float te : {0.5f,1e6f}){
    std::vector<float> pos,attr; std::vector<unsigned> idx;
    for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n,z=0;if(i==n/2&&j==n/2)z=h;pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
    for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
    std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
    size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,te,0,&err,mse,d,&st);
    if(st.rejected_distance>0) printf("HIT n=%u h=%.1f mse=%g d=%u te=%.1f out=%zu acc=%zu rejD=%zu rejB=%zu cert=%g\n",n,h,mse,d,te,c,st.accepted_collapses,st.rejected_distance,st.rejected_budget,st.certified_error);
  }
  puts("done");
  return 0;
}
