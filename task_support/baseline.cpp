#include "DspFilters/Dsp.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
static std::vector<double> render(const std::vector<double>& x, bool split) {
  Dsp::RBJ::LowPass stages[2];
  stages[0].setup(48000,1300,0.707); stages[1].setup(48000,4100,1.2);
  Dsp::BiquadBase::State<Dsp::DirectFormII> state[2];
  auto y=x; size_t n=0;
  while(n<y.size()) {
    const size_t k=std::min(y.size()-n,split?size_t(1+n%31):y.size());
    for(int s=0;s<2;++s) stages[s].process(int(k),y.data()+n,state[s]);
    n+=k;
  }
  return y;
}
int main(){
  const size_t n=4097; double energy=0,maxdiff=0,stereoDiff=0;
  for(int c=0;c<2;++c){
    std::vector<double> x(n);
    for(size_t i=0;i<n;++i) x[i]=0.3*std::sin((0.017+0.013*c)*i)+(i==size_t(17+c*13)?0.8:0.0);
    auto a=render(x,false), b=render(x,true), again=render(x,false);
    for(size_t i=0;i<n;++i){
      if(!std::isfinite(a[i])||a[i]!=again[i])return 2;
      energy+=a[i]*a[i]; maxdiff=std::max(maxdiff,std::abs(a[i]-b[i]));
      stereoDiff+=std::abs(a[i]-x[i]);
    }
  }
  if(!(energy>1 && stereoDiff>1 && maxdiff<1e-14))return 3;
  std::printf("DSPFILTERS_BASELINE_OK frames=%zu channels=2 stages=2 energy=%.17g chunk_error=%.17g\n",n,energy,maxdiff);
  return 0;
}
