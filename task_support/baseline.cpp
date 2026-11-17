#include "src/RKSimulationModel.h"
#include "src/Oversampler.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
static std::vector<float> oldRender(const std::vector<float>& x,bool split){
  RKSimulationMoog f(48000); f.SetCutoff(2300);f.SetResonance(2.8f);
  auto y=x;size_t pos=0;
  while(pos<y.size()){
    size_t n=std::min(y.size()-pos,split?size_t(1+pos%29):y.size());
    f.Process(y.data()+pos,(uint32_t)n);pos+=n;
  }
  return y;
}
static std::vector<float> fixedResample(const std::vector<float>& x,bool split){
  MoogLadders::Oversampling::Oversampler<4> os;
  std::vector<float> y(x.size()),u(x.size()*4);size_t pos=0;
  while(pos<x.size()){
    size_t n=std::min(x.size()-pos,split?size_t(1+pos%31):x.size());
    os.ProcessUp(x.data()+pos,u.data(),(uint32_t)n);
    os.ProcessDown(u.data(),y.data()+pos,(uint32_t)n);pos+=n;
  }
  return y;
}
int main(){
  const size_t n=2053;double energy=0,rkDiff=0,osDiff=0;
  for(int ch=0;ch<2;++ch){
    std::vector<float>x(n);
    for(size_t i=0;i<n;++i)x[i]=float(0.7*std::sin((0.03+0.017*ch)*i)+(i==size_t(7+11*ch)?2:0));
    auto a=oldRender(x,false),b=oldRender(x,true),again=oldRender(x,false);
    auto c=fixedResample(x,false),d=fixedResample(x,true);
    for(size_t i=0;i<n;++i){
      if(!std::isfinite(a[i])||!std::isfinite(c[i])||a[i]!=again[i])return 2;
      energy+=a[i]*a[i];rkDiff=std::max(rkDiff,double(std::abs(a[i]-b[i])));
      osDiff=std::max(osDiff,double(std::abs(c[i]-d[i])));
    }
  }
  if(!(energy>1 && rkDiff==0 && osDiff==0))return 3;
  std::printf("MOOG_BASELINE_OK frames=%zu channels=2 energy=%.17g rk_chunk_error=%.17g oversample_chunk_error=%.17g\n",n,energy,rkDiff,osDiff);
  return 0;
}
