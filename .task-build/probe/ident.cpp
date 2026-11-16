#include "src/meshoptimizer.h"
#include <stdio.h>
#include <string.h>
#include <vector>
int main(){
  unsigned n=8; std::vector<float> pos,attr; std::vector<unsigned> idx;
  for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n,z=(i==n/2&&j==n/2)?0.2f:0;pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
  for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
  unsigned opts[]={0u,(unsigned)meshopt_SimplifyLockBorder,(unsigned)meshopt_SimplifySparse,(unsigned)meshopt_SimplifyErrorAbsolute,(unsigned)(meshopt_SimplifySparse|meshopt_SimplifyLockBorder|meshopt_SimplifyErrorAbsolute)};
  int fail=0;
  for(unsigned oi=0;oi<5;++oi)for(float te : {0.01f, 0.5f, 1.f}){
    std::vector<unsigned> a(idx.size()),b(idx.size()); float ea=-1,eb=-1; meshopt_SurfaceErrorStats st; memset(&st,0xAA,sizeof(st));
    float w[2]={0.1f,0.1f};
    size_t ca=meshopt_simplifyWithSurfaceError(a.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,64*3,te,opts[oi],&ea,-1.f,0,&st);
    size_t cb=meshopt_simplifyWithAttributes(b.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,64*3,te,opts[oi],&eb);
    int ok=ca==cb && ea==eb && memcmp(a.data(),b.data(),ca*4)==0 && st.accepted_collapses==0&&st.distance_tests==0&&st.certified_error==0&&st.rejected_distance==0&&st.rejected_budget==0&&st.invalidated_certificates==0;
    printf("opt=%u te=%.2f count %zu/%zu err %g/%g %s\n",opts[oi],te,ca,cb,ea,eb,ok?"OK":"MISMATCH");
    if(!ok)fail++;
  }
  return fail;
}
