#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
#include <math.h>
static void test(unsigned n,int block,float h,float mse,float te,unsigned d){
  std::vector<float> pos,attr; std::vector<unsigned> idx;
  for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n,z=0;int ci=int(i)-int(n/2),cj=int(j)-int(n/2);if(abs(ci)<=block&&abs(cj)<=block)z=h;pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
  for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
  std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
  size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,te,0,&err,mse,d,&st);
  printf("n=%u block=%d h=%.1f mse=%.3f te=%.1f d=%u out=%zu acc=%zu rejD=%zu rejB=%zu cert=%g\n",n,block,h,mse,te,d,c,st.accepted_collapses,st.rejected_distance,st.rejected_budget,st.certified_error);
}
int main(){
  for(unsigned n=8;n<=12;n+=4)for(int b=0;b<=1;++b)for(float h=0.3f;h<=0.9f;h+=0.3f)for(float te : {0.5f,4.f}) test(n,b,h,0.01f,te,16);
  return 0;
}
