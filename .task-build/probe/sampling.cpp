#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
int main(){
  for(int shells=16;shells<=64;shells*=4){
    std::vector<float> pos,attr; std::vector<unsigned> idx; unsigned n=4;
    for(int s=0;s<shells;++s){float ox=float(s%8)*100.f,oy=float(s/8)*100.f;unsigned base=unsigned(pos.size()/3);
      for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){pos.push_back(ox+float(i)/n);pos.push_back(oy+float(j)/n);pos.push_back(0);attr.push_back(float(i)/n);attr.push_back(float(j)/n);}
      for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=base+j*(n+1)+i,b=a+1,c=a+n+1,d=c+1;unsigned f[]={a,b,d,a,d,c};idx.insert(idx.end(),f,f+6);}
    }
    std::vector<unsigned> out(idx.size()); float w[2]={0.1f,0.1f}; float err=-1; meshopt_SurfaceErrorStats st={};
    size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,size_t(shells)*2*3,0.5f,0,&err,0.01f,10,&st);
    printf("shells=%d in=%zu out=%zu tri=%zu acc=%zu tests=%zu per=%.1f cert=%g\n",shells,idx.size(),c,c/3,st.accepted_collapses,st.distance_tests,double(st.distance_tests)/st.accepted_collapses,st.certified_error);
  }
  return 0;
}
