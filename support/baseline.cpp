#include "TwoStageFFTConvolver.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
struct Observed : fftconvolver::TwoStageFFTConvolver {
  size_t jobs=0;
  void startBackgroundProcessing() override { ++jobs; doBackgroundProcessing(); }
};
int main() {
  const size_t inLen=257, irLen=401, outLen=inLen+irLen-1;
  std::vector<float> in(outLen,0), ir(irLen), out(outLen);
  for(size_t i=0;i<inLen;++i) in[i]=float(int(i%17)-8)/32;
  for(size_t i=0;i<irLen;++i) ir[i]=float(int((i*7)%19)-9)/64;
  Observed c;
  if(!c.init(8,64,ir.data(),ir.size())) return 1;
  size_t calls=0,idle=0,maxJobs=0;
  for(size_t pos=0;pos<outLen;) {
    size_t n=std::min(size_t(8),outLen-pos), before=c.jobs;
    c.process(in.data()+pos,out.data()+pos,n);
    size_t now=c.jobs-before;
    idle+=(now==0);maxJobs=std::max(maxJobs,now);++calls;pos+=n;
  }
  double maxError=0;
  for(size_t n=0;n<outLen;++n) {
    double ref=0;
    for(size_t k=0;k<irLen && k<=n;++k) ref+=double(ir[k])*in[n-k];
    double e=std::fabs(double(out[n])-ref);if(!std::isfinite(out[n]) || e>1e-4) return 2;
    maxError=std::max(maxError,e);
  }
  std::printf("BASELINE_PCM_OK samples=%zu max_error=%.9g\n",outLen,maxError);
  std::printf("BASELINE_TAIL_BATCH calls=%zu launches=%zu idle=%zu max_jobs=%zu\n",calls,c.jobs,idle,maxJobs);
  if(c.jobs==0 || idle==0) return 3;
}
