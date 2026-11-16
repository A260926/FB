#include "src/meshoptimizer.h"
#include <stdio.h>
#include <math.h>
#include <vector>
struct V { float x,y,z,u,v; };
int main(){
  const unsigned n=16;
  std::vector<V> verts; std::vector<unsigned> idx;
  for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){V v={float(i)/n,float(j)/n,0,float(i)/n,float(j)/n};verts.push_back(v);}
  for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,d=c+1;unsigned f[]={a,b,d,a,d,c};idx.insert(idx.end(),f,f+6);}
  std::vector<unsigned> out(idx.size());
  float w[2]={0.1f,0.1f}; float err=-1;
  meshopt_SurfaceErrorStats st={};
  size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),&verts[0].x,verts.size(),sizeof(V),&verts[0].u,sizeof(V),w,2,NULL,128*3,1.f,meshopt_SimplifyLockBorder|meshopt_SimplifyErrorAbsolute,&err,.01f,16,&st);
  printf("out=%zu tri=%zu err=%g certified=%g acc=%zu rejD=%zu rejB=%zu inv=%zu tests=%zu\n",c,c/3,err,st.certified_error,st.accepted_collapses,st.rejected_distance,st.rejected_budget,st.invalidated_certificates,st.distance_tests);
  std::vector<unsigned> out2(idx.size()); float err2=-1;
  size_t c2=meshopt_simplifyWithAttributes(out2.data(),idx.data(),idx.size(),&verts[0].x,verts.size(),sizeof(V),&verts[0].u,sizeof(V),w,2,NULL,128*3,1.f,meshopt_SimplifyLockBorder|meshopt_SimplifyErrorAbsolute,&err2);
  printf("ref out=%zu err=%g\n",c2,err2);
  return 0;
}
