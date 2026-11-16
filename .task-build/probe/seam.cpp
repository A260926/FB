#include "src/meshoptimizer.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <math.h>
int main(){
  std::vector<float> pos; std::vector<float> attr; std::vector<unsigned> idx;
  unsigned n=6;
  for(unsigned j=0;j<=n;++j)for(int side=0;side<2;++side)for(unsigned i=0;i<=n;++i){
    float x=float(i)/n,y=float(j)/n;
    pos.push_back(x);pos.push_back(y);pos.push_back(0);
    attr.push_back(x);attr.push_back(y+0.5f*side);
  }
  for(unsigned j=0;j<n;++j)for(int side=0;side<2;++side)for(unsigned i=0;i<n;++i){
    unsigned base=(j*2+side)*(n+1); unsigned a=base+i,b=a+1,c=base+2*(n+1),d=c+1;
    unsigned f[]={a,b,d,a,d,c}; idx.insert(idx.end(),f,f+6);
  }
  std::vector<unsigned> out(idx.size()); float w[2]={0.1f,0.1f}; float err=-1; meshopt_SurfaceErrorStats st={};
  size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,64*3,0.1f,0,&err,0.01f,16,&st);
  printf("count=%zu tri=%zu cert=%g acc=%zu rejD=%zu rejB=%zu\n",c,c/3,st.certified_error,st.accepted_collapses,st.rejected_distance,st.rejected_budget);
  for(size_t i=0;i<c;i+=3){
    unsigned a=out[i],b=out[i+1],d=out[i+2];
    double p0x=pos[a*3],p0y=pos[a*3+1],p0z=pos[a*3+2];
    double p1x=pos[b*3],p1y=pos[b*3+1],p1z=pos[b*3+2];
    double p2x=pos[d*3],p2y=pos[d*3+1],p2z=pos[d*3+2];
    double ax=p1x-p0x,ay=p1y-p0y,az=p1z-p0z,bx=p2x-p0x,by=p2y-p0y,bz=p2z-p0z;
    double nx=ay*bz-az*by,ny=az*bx-ax*bz,nz=ax*by-ay*bx;
    double ar=sqrt(nx*nx+ny*ny+nz*nz);
    if(!(ar>0)) printf("degenerate tri %zu: %u %u %u area=%g\n",i/3,a,b,d,ar);
  }
  return 0;
}
