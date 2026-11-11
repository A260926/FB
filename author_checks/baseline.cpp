#include "rubberband/RubberBandLiveShifter.h"
#include <vector>
#include <cmath>
#include <cassert>
#include <cstdio>
using RubberBand::RubberBandLiveShifter;
int main() {
  RubberBandLiveShifter s(48000,2,RubberBandLiveShifter::OptionChannelsTogether);
  unsigned bs=s.getBlockSize(), delay=s.getStartDelay(), n=bs*96;
  std::vector<float> l(n),r(n),ol(n),orr(n);
  for(unsigned i=0;i<n;i++) {l[i]=.3f*std::sin(2*3.141592653589793*440*i/48000); r[i]=-l[i];}
  for(unsigned i=0;i<n;i+=bs) {
    const float *in[]={l.data()+i,r.data()+i}; float *out[]={ol.data()+i,orr.data()+i}; s.shift(in,out);
  }
  double error=0,side=0,energy=0;
  for(unsigned i=4096;i+delay<n;i++) {
    error=std::max(error,double(std::abs(ol[i+delay]-l[i])));
    side=std::max(side,double(std::abs(ol[i+delay]+orr[i+delay])));
    energy+=ol[i+delay]*ol[i+delay];
  }
  assert(energy>1 && error<.01 && side<1e-6);
  std::printf("LIVE_UNITY_STEREO_OK block=%u delay=%u maxerr=%.8f antisym=%.8f\n",bs,delay,error,side);
}
