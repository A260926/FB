#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
#include <math.h>
// roof: grid over x in [0,1], y in [0,1], z = h*(1 - |2y-1|)  -> ridge along y=0.5
static size_t run(unsigned n,float h,float mse,unsigned d,float te){
  std::vector<float> pos,attr; std::vector<unsigned> idx;
  for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n;float z=h*(1.f-fabsf(2.f*y-1.f));pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
  for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
  std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
  size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,te,0,&err,mse,d,&st);
  printf("n=%u h=%.2f mse=%.3f d=%u te=%.1f in=%zu out=%zu acc=%zu rejD=%zu rejB=%zu cert=%g qem=%g\n",n,h,mse,d,te,idx.size(),c,st.accepted_collapses,st.rejected_distance,st.rejected_budget,st.certified_error,err);
  return st.rejected_distance;
}
int main(){
  size_t total=0;
  for(unsigned n=6;n<=12;n+=2)for(float h=0.2f;h<=0.6f;h+=0.2f)for(float te=0.5f;te<=8.f;te*=4.f)for(unsigned d=8;d<=16;d+=4) total+=run(n,h,0.01f,d,te);
  printf("total_rejD=%zu\n",total);
  return 0;
}
