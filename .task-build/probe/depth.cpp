#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
int main(){
  for(unsigned n=6;n<=10;n+=2){
    std::vector<float> pos,attr; std::vector<unsigned> idx;
    for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n;pos.push_back(x);pos.push_back(y);pos.push_back(0);attr.push_back(x);attr.push_back(y);}
    for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,d=c+1;unsigned f[]={a,b,d,a,d,c};idx.insert(idx.end(),f,f+6);}
    std::vector<unsigned> out(idx.size()); float w[2]={0.1f,0.1f}; float err=-1; meshopt_SurfaceErrorStats st={};
    size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,32*3,0.5f,0,&err,0.01f,1,&st);
    printf("n=%u in=%zu count=%zu tri=%zu cert=%g acc=%zu rejD=%zu rejB=%zu\n",n,idx.size(),c,c/3,st.certified_error,st.accepted_collapses,st.rejected_distance,st.rejected_budget);
  }
  return 0;
}
