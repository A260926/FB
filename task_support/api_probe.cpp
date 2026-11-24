#include "rubberband/R2RenderIndex.h"
#include "rubberband/RubberBandStretcher.h"
#include "task_support/fft_observer.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>
#include <cstdio>
using S=RubberBand::RubberBandStretcher;using I=RubberBand::R2RenderIndex;
struct Source{std::vector<float>x;size_t reads=0;};
static bool readAt(void*ctx,size_t first,size_t count,float*const*out){auto&s=*static_cast<Source*>(ctx);if(first>s.x.size()||count>s.x.size()-first)return false;s.reads+=count;std::copy_n(s.x.data()+first,count,out[0]);return true;}
static bool failRead(void*,size_t,size_t,float*const*){return false;}
int main(){
 Source src;src.x.resize(24000);for(size_t i=0;i<src.x.size();++i)src.x[i]=float(.2*std::sin(.0576*i)+(i%4000<16?.3:0));
 std::map<size_t,size_t>map{{0,0},{12000,8000},{24000,30000}};
 size_t inv=rb_task_fft_inverse_count();auto idx=I::build(48000,1,src.x.size(),1.25,4096,map,readAt,&src);
 if(!idx||rb_task_fft_inverse_count()!=inv||src.reads==0||idx->getInputFrames()!=24000||idx->getOutputFrames()!=30000)return 11;
 std::ostringstream bytes(std::ios::binary);idx->save(bytes);if(bytes.str().empty())return 12;
 std::istringstream input(bytes.str(),std::ios::binary);auto loaded=I::load(input);if(!loaded||loaded->getOutputFrames()!=30000)return 13;idx.reset();
 S s(48000,1,S::OptionEngineFaster|S::OptionThreadingNever|S::OptionPhaseIndependent,1.25,1);s.setExpectedInputDuration(24000);s.setMaxProcessSize(4096);s.setKeyFrameMap(map);
 const float*p=src.x.data();s.study(&p,src.x.size(),true);std::vector<float>ref;
 for(size_t i=0;i<src.x.size();){size_t k=std::min<size_t>(4096,src.x.size()-i);p=src.x.data()+i;s.process(&p,k,i+k==src.x.size());i+=k;while(s.available()>0){std::vector<float>b(s.available());float*q=b.data();size_t got=s.retrieve(&q,b.size());if(!got)return 14;ref.insert(ref.end(),b.begin(),b.begin()+got);}}
 if(ref.size()!=30000)return 15;
 std::vector<float>out(30000,-99.f);const size_t starts[]={16000,0,8000,24000};
 for(size_t st:starts){size_t count=std::min<size_t>(8000,30000-st);float*q=out.data()+st;size_t before=src.reads;if(!loaded->render(st,count,readAt,&src,&q))return 16;if(src.reads-before>32*(4096+count+8192))return 17;}
 double energy=0;for(size_t i=0;i<out.size();++i){if(!std::isfinite(out[i])||std::fabs(out[i]-ref[i])>2e-5)return 18;energy+=out[i]*out[i];}if(energy<1)return 19;
 float untouched[7];std::fill_n(untouched,7,19.f);float*q=untouched;
 if(loaded->render(30001,7,readAt,&src,&q)||loaded->render(1000,7,failRead,nullptr,&q))return 20;for(float f:untouched)if(f!=19.f)return 21;
 size_t before=src.reads;if(!loaded->render(30000,0,readAt,&src,&q)||src.reads!=before)return 22;
 std::puts("R2_RENDER_INDEX_API_OK");
}
