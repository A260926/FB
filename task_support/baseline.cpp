#include "Cpp/Source/Test.h"
#include <cmath>
#include <cstdio>
#include <vector>
int main(){const int w=16,h=12;std::vector<float> pixels(w*h*4,0);InitializeTest();int rays=0,totalRays=0;for(int f=0;f<2;++f){UpdateTest(0,f,w,h,kFlagProgressive);DrawTest(0,f,w,h,pixels.data(),rays,kFlagProgressive);if(rays<=0)return 2;totalRays+=rays;}double energy=0;for(int i=0;i<w*h;++i)for(int c=0;c<3;++c){float v=pixels[i*4+c];if(!std::isfinite(v)||v<0)return 3;energy+=v;}ShutdownTest();if(energy<=0)return 4;std::printf("BASELINE CPU frames=2 rays=%d rgb_sum=%.12g\n",totalRays,energy);std::puts("PRODUCTION_BASELINE_OK");}
