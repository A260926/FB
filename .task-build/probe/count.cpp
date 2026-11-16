#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
int main(){
  unsigned n=12; std::vector<float> pos,attr; std::vector<unsigned> idx;
  for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n,z=0;pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
  for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
  std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
  size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,1e6f,0,&err,0.01f,16,&st);
  printf("in_tri=%zu out_tri=%zu removed=%zu accepted=%zu (each collapse removes 1 or 2)\n",idx.size()/3,c/3,idx.size()/3-c/3,st.accepted_collapses);
  return 0;
}
