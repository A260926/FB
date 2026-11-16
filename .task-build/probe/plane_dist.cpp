#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
int main(){
  // 1x1 plane (2 triangles); a collapse removes one triangle, leaving a triangle that covers only half -- the
  // remaining surface is 0.5 away from the removed corner. Initial T==S so first collapses are exact; after a
  // collapse the active surface is half, so S->T distance becomes large and must be rejected.
  for(float mse : {0.01f, 0.2f, 0.6f}){
    unsigned n=1; std::vector<float> pos,attr; std::vector<unsigned> idx;
    for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i),y=float(j),z=0;pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
    for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
    std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
    size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,1e6f,0,&err,mse,16,&st);
    printf("mse=%.2f out=%zu acc=%zu rejD=%zu rejB=%zu cert=%g\n",mse,c,st.accepted_collapses,st.rejected_distance,st.rejected_budget,st.certified_error);
  }
  return 0;
}
