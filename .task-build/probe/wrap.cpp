#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
int main(){
  unsigned n=6; std::vector<float> pos,attr; std::vector<unsigned short> idx16;
  for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){float x=float(i)/n,y=float(j)/n;pos.push_back(x);pos.push_back(y);pos.push_back(0);attr.push_back(x);attr.push_back(y);}
  for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned short f[]={(unsigned short)a,(unsigned short)b,(unsigned short)dd,(unsigned short)a,(unsigned short)dd,(unsigned short)c};idx16.insert(idx16.end(),f,f+6);}
  std::vector<unsigned short> out16(idx16.size()); float w[2]={0.1f,0.1f}; float err=-1; meshopt_SurfaceErrorStats st={};
  size_t c=meshopt_simplifyWithSurfaceError(out16.data(), idx16.data(), idx16.size(), pos.data(), pos.size()/3, 12, attr.data(), 8, w, 2, NULL, 0, 1e6f, 0, &err, 0.01f, 12, &st);
  int ok=c>0; for(size_t i=0;i<c;++i) if(out16[i]>=pos.size()/3) ok=0;
  printf("16bit count=%zu ok=%d cert=%g\n",c,ok,st.certified_error);
  // default-argument wrapper (disabled) - just check it compiles and matches
  std::vector<unsigned short> d(idx16.size());
  size_t cd=meshopt_simplifyWithSurfaceError(d.data(), idx16.data(), idx16.size(), pos.data(), pos.size()/3, 12, attr.data(), 8, w, 2, NULL, 18, 0.5f);
  printf("defaults count=%zu\n",cd);
  return 0;
}
