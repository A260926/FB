#include "src/meshoptimizer.h"
#include <stdio.h>
#include <vector>
#include <math.h>
// rotated + translated plane with bump; verifies object-space bound is frame independent
int main(){
  for(float ang : {0.f, 0.4f, 1.1f}) for(float off : {0.f, 5.f, -3.f}){
    unsigned n=8; std::vector<float> pos,attr; std::vector<unsigned> idx;
    float ca=cosf(ang), sa=sinf(ang);
    for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i){
      float x=float(i)/n, y=float(j)/n, z=0.2f*(i==n/2&&j==n/2);
      float rx=ca*x - sa*y + off, ry=sa*x + ca*y - off, rz=z;
      pos.push_back(rx);pos.push_back(ry);pos.push_back(rz);attr.push_back(x);attr.push_back(y);
    }
    for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){unsigned a=j*(n+1)+i,b=a+1,c=a+n+1,dd=c+1;unsigned f[]={a,b,dd,a,dd,c};idx.insert(idx.end(),f,f+6);}
    std::vector<unsigned> out(idx.size());float w[2]={0.1f,0.1f};float err=-1;meshopt_SurfaceErrorStats st={};
    size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,0,1e6f,0,&err,0.01f,16,&st);
    printf("ang=%.1f off=%.0f out=%zu cert=%g reduce=%d\n",ang,off,c,st.certified_error,int(c<idx.size()));
  }
  return 0;
}
