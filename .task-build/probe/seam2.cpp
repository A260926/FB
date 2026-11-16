#include "src/meshoptimizer.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <math.h>
#include <assert.h>
int main(){
  std::vector<float> pos,attr; std::vector<unsigned> idx; unsigned n=6;
  std::vector<unsigned> row_base(n+1); unsigned per_row=0;
  for(unsigned i=0;i<=n;++i){row_base[i]=per_row;per_row+=(i==n/2?2:1);}
  for(unsigned j=0;j<=n;++j)for(unsigned i=0;i<=n;++i)for(int copy=0;copy<=(i==n/2?1:0);++copy){
    float x=float(i)/n,y=float(j)/n; pos.push_back(x);pos.push_back(y);pos.push_back(0);attr.push_back(x+0.25f*copy);attr.push_back(y);
  }
  for(unsigned j=0;j<n;++j)for(unsigned i=0;i<n;++i){
    unsigned left_copy=i<n/2?0:1, right_copy=i+1<=n/2?0:1;
    if(i==n/2)continue;
    unsigned a=j*per_row+row_base[i]+(i==n/2?left_copy:0);
    unsigned b=j*per_row+row_base[i+1]+(i+1==n/2?right_copy:0);
    unsigned c=(j+1)*per_row+row_base[i]+(i==n/2?left_copy:0);
    unsigned d=(j+1)*per_row+row_base[i+1]+(i+1==n/2?right_copy:0);
    unsigned f[]={a,b,d,a,d,c}; idx.insert(idx.end(),f,f+6);
  }
  printf("verts=%zu tris=%zu\n",pos.size()/3,idx.size()/3);
  for(size_t i=0;i<idx.size();i+=3){
    unsigned a=idx[i],b=idx[i+1],c=idx[i+2];
    double p0x=pos[a*3],p0y=pos[a*3+1],p1x=pos[b*3],p1y=pos[b*3+1],p2x=pos[c*3],p2y=pos[c*3+1];
    double ar=(p1x-p0x)*(p2y-p0y)-(p1y-p0y)*(p2x-p0x);
    if(fabs(ar)<1e-12)printf("bad %zu: %u %u %u\n",i/3,a,b,c);
  }
  {float ww[2]={0.1f,0.1f}; std::vector<unsigned> t(idx); float e2=-1; std::vector<unsigned> o2(idx.size()); meshopt_SurfaceErrorStats s2={}; size_t cc=meshopt_simplifyWithAttributes(o2.data(),t.data(),t.size(),pos.data(),pos.size()/3,12,attr.data(),8,ww,2,NULL,32*3,0.1f,0,&e2); printf("ref count=%zu err=%g\n",cc,e2);}
  std::vector<unsigned> out(idx.size()); float w[2]={0.1f,0.1f}; float err=-1; meshopt_SurfaceErrorStats st={};
  size_t c=meshopt_simplifyWithSurfaceError(out.data(),idx.data(),idx.size(),pos.data(),pos.size()/3,12,attr.data(),8,w,2,NULL,32*3,0.1f,0,&err,0.01f,16,&st);
  printf("count=%zu tri=%zu cert=%g acc=%zu rejD=%zu rejB=%zu\n",c,c/3,st.certified_error,st.accepted_collapses,st.rejected_distance,st.rejected_budget);
  for(size_t i=0;i<c;i+=3){
    unsigned a=out[i],b=out[i+1],d=out[i+2];
    if(a>=pos.size()/3||b>=pos.size()/3||d>=pos.size()/3){printf("oob %zu\n",i/3);continue;}
    double p0x=pos[a*3],p0y=pos[a*3+1],p1x=pos[b*3],p1y=pos[b*3+1],p2x=pos[d*3],p2y=pos[d*3+1];
    double ar=(p1x-p0x)*(p2y-p0y)-(p1y-p0y)*(p2x-p0x);
    if(fabs(ar)<1e-12)printf("out bad %zu: %u %u %u ar=%g\n",i/3,a,b,d,ar);
  }
  return 0;
}
