#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
int main(){
  unsigned n=8; std::vector<float> pos,attr; std::vector<unsigned> idx;
  for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n,z=0;pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
  for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
  std::vector<unsigned> o1(idx.size()),o2(idx.size()); float w[2]={0.1f,0.1f},e1=-1,e2=-1; meshopt_SurfaceErrorStats st={};
  size_t c1=meshopt_simplifyWithSurfaceError(o1.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,1e6f,0,&e1,0.01f,16,NULL);
  size_t c2=meshopt_simplifyWithSurfaceError(o2.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,1e6f,0,&e2,0.01f,16,&st);
  int same = c1==c2 && e1==e2 && memcmp(o1.data(),o2.data(),c1*4)==0;
  printf("statsNULL count=%zu same_as_with_stats=%d cert=%g\n",c1,same,st.certified_error);
  return 0;
}
