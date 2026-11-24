#include "rubberband/RubberBandStretcher.h"
#include "task_support/fft_observer.h"
#include <vector>
#include <map>
#include <cmath>
#include <cstdio>
#include <algorithm>
using S=RubberBand::RubberBandStretcher;
static std::vector<float> run(const std::vector<float>&x,double ratio,const std::map<size_t,size_t>&m){
 S s(48000,1,S::OptionEngineFaster|S::OptionThreadingNever|S::OptionPhaseIndependent,ratio,1);
 s.setExpectedInputDuration(x.size());s.setMaxProcessSize(4096);s.setKeyFrameMap(m);
 const float*p=x.data();s.study(&p,x.size(),true);std::vector<float>y;
 for(size_t i=0;i<x.size();){size_t k=std::min<size_t>(4096,x.size()-i);p=x.data()+i;s.process(&p,k,i+k==x.size());i+=k;
 while(s.available()>0){std::vector<float>b(s.available());float*q=b.data();size_t got=s.retrieve(&q,b.size());if(!got)throw 2;y.insert(y.end(),b.begin(),b.begin()+got);}}
 if(s.available()!=-1)throw 3;return y;
}
int main(){
 const size_t n=24000;std::vector<float>x(n);
 for(size_t i=0;i<n;++i)x[i]=float(.2*std::sin(i*.0576)+.11*std::cos(i*.0331)+(i%4000<16?.3:0));
 auto whole=run(x,1.25,{{0,0},{12000,8000},{n,30000}});
 std::vector<float>a(x.begin(),x.begin()+12000),b(x.begin()+12000,x.end());
 auto u=run(a,8000./12000.,{{0,0},{12000,8000}});auto v=run(b,22000./12000.,{{0,0},{12000,22000}});
 if(whole.size()!=30000||u.size()!=8000||v.size()!=22000)return 4;u.insert(u.end(),v.begin(),v.end());
 double energy=0,delta=0;for(size_t i=0;i<whole.size();++i){if(!std::isfinite(whole[i]))return 5;energy+=whole[i]*whole[i];delta=std::max(delta,double(std::fabs(whole[i]-u[i])));}
 if(energy<1||delta<1e-3||rb_task_fft_inverse_count()==0)return 6;
 std::printf("R2_PARTITION_BASELINE_OK input=%zu output=%zu energy=%.6f inverse_ffts=%zu\n",n,whole.size(),energy,rb_task_fft_inverse_count());
 std::printf("R2_INDEPENDENT_CUT_DIFF max_abs=%.9f\n",delta);
}
