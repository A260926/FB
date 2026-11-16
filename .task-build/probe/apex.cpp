#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
int main(){
  // 2x2 grid (3x3 verts) with center apex; target 2 triangles forces removing the apex
  for(unsigned d=2; d<=14; d+=2){
    unsigned n=2;
    std::vector<float> pos,attr; std::vector<unsigned> idx;
    for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n,z=0;if(i==1&&j==1)z=0.5f;pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
    for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
    std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
    size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,1e6f,0,&err,0.01f,d,&st);
    printf("d=%u out=%zu acc=%zu rejD=%zu rejB=%zu cert=%g\n",d,c,st.accepted_collapses,st.rejected_distance,st.rejected_budget,st.certified_error);
  }
  return 0;
}
