#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
int main(){
  for(float scale : {1.f, 100.f}){
    unsigned n=8; std::vector<float> pos,attr; std::vector<unsigned> idx;
    for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n*scale,y=float(j)/n*scale,z=0;pos.push_back(x);pos.push_back(y);pos.push_back(z);attr.push_back(x);attr.push_back(y);}
    for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
    for(unsigned opt : {0u,(unsigned)meshopt_SimplifyErrorAbsolute}){
      std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
      size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,1e6f,opt,&err,0.5f,16,&st);
      printf("scale=%g opt=%u out=%zu cert=%g (<=0.5? %d) qem=%g\n",scale,opt,c,st.certified_error,int(st.certified_error<=0.5+1e-9),err);
    }
  }
  return 0;
}
