#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
// a mesh with many unused vertices before/after, and a subset of triangles; sparse indices refer to a large vertex buffer
int main(){
  unsigned n=8; unsigned pad=100;
  std::vector<float> pos,attr;
  for(unsigned k=0;k<pad;++k){pos.push_back(-999);pos.push_back(-999);pos.push_back(-999);attr.push_back(0);attr.push_back(0);}
  for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n,z=0;pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
  std::vector<unsigned> idx;
  for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=pad+j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
  std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
  size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,1e6f,meshopt_SimplifySparse,&err,0.01f,16,&st);
  int oob=0; for(size_t i=0;i<c;++i) if(out[i]>=pos.size()/3) oob=1;
  printf("count=%zu oob=%d cert=%g acc=%zu qem=%g\n",c,oob,st.certified_error,st.accepted_collapses,err);
  return 0;
}
