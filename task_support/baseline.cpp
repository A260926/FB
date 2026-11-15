#include "Test.h"
#include <vector>
#include <cmath>
#include <cstdio>
int main(){
  const int w=16,h=12; std::vector<float> pixels(w*h*4,0);
  InitializeTest(); UpdateTest(0,0,w,h,0);
  int rays=0; DrawTest(0,0,w,h,pixels.data(),rays,0); ShutdownTest();
  double sum=0; for(int i=0;i<w*h;i++) for(int c=0;c<3;c++) {
    float v=pixels[i*4+c]; if(!std::isfinite(v)||v<0) return 2; sum+=v;
  }
  if(rays<=w*h||sum<=0) return 3;
  std::printf("CPU_RENDER_OK pixels=%d rays=%d linear_sum=%.9f\n",w*h,rays,sum);
  return 0;
}
