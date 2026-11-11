#include "Test.h"
#include <vector>
#include <cstdio>
#include <cmath>
#include <cassert>
int main(){
 const int w=20,h=12;std::vector<float>a(w*h*4,0),b(a.size(),0);
 InitializeTest();UpdateTest(0,0,w,h,0);int ra=0,rb=0;
 DrawTest(0,0,w,h,a.data(),ra,0);DrawTest(0,0,w,h,b.data(),rb,0);
 double sum=0;int different=0;
 for(int p=0;p<w*h;p++)for(int k=0;k<3;k++){
  const int i=p*4+k;assert(std::isfinite(a[i])&&a[i]>=0);sum+=a[i];
  if(a[i]!=b[i])different++;
 }
 assert(ra>0&&ra==rb&&different==0&&sum>1);
 printf("FRAME_BASELINE size=%dx%d rays=%d rgb_sum=%.8f repeat_diff=%d\n",w,h,ra,sum,different);
 ShutdownTest();puts("TOY_RENDER_BASELINE_OK");
}
