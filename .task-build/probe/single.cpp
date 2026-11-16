#include "src/meshoptimizer.h"
#include <stdio.h>
int main(){
  float pos[]={0,0,0, 1,0,0, 0,1,0}; float attr[]={0,0, 1,0, 0,1}; unsigned idx[]={0,1,2};
  unsigned out[3]; float w[2]={0.1f,0.1f}; float err=-1; meshopt_SurfaceErrorStats st={};
  size_t c=meshopt_simplifyWithSurfaceError(out,idx,3,pos,3,12,attr,8,w,2,NULL,0,1e6f,0,&err,1.f,10,&st);
  printf("single tri -> count=%zu acc=%zu rejD=%zu rejB=%zu cert=%g\n",c,st.accepted_collapses,st.rejected_distance,st.rejected_budget,st.certified_error);
  return 0;
}
