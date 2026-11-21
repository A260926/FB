#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifndef IQM_HEADLESS
#define IQM_HEADLESS
#endif
#include "demo/demo.cpp"
int main(int argc,char**argv) {
 if(argc!=2 || !loadiqm(argv[1]) || numjoints!=2 || numverts!=3 || numframes!=2) return 1;
 animateiqm(0.5f);
 for(int i=0;i<numverts;++i) {
  if(std::fabs(outposition[3*i]-inposition[3*i]-1)>2e-5f || std::fabs(outposition[3*i+1]-inposition[3*i+1])>2e-5f || std::fabs(outnormal[3*i+2]-1)>2e-5f || std::fabs(outtangent[3*i]-1)>2e-5f || std::fabs(outbitangent[3*i+1]-1)>2e-5f) return 2;
 }
 std::printf("SERIAL_SKIN_OK joints=%d vertices=%d frames=%d dx=1 directions=1\n",numjoints,numverts,numframes);
 cleanupiqm(); if(animdata && animdata!=meshdata) delete[] animdata; delete[] meshdata; return 0;
}
