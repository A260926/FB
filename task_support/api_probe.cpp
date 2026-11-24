#include "sequence/Ducking.h"
#include "sequence/Sequence.h"
#include "sequence/SequenceEntry.h"
#include "probe_sound.h"
#include <cmath>
#include <iostream>
#include <vector>
static std::vector<float> render(bool enabled){
 aud::Sequence s({8000,aud::CHANNELS_STEREO},25,false);
 auto t=s.add(std::make_shared<ProbeSound>(.25f),0,.25,0);
 auto k=s.add(std::make_shared<ProbeSound>(0),0,.25,0);
 if(enabled){aud::DuckingSettings d{.5f,2,0,0,7};s.setDucking(t,k,d);s.clearDucking(t);s.setDucking(t,k,d);}
 auto r=s.createQualityReader(aud::ResampleQuality::FASTEST);
 std::vector<float> out(512);int n=256;bool eos=false;r->read(n,eos,out.data());
 if(n!=256)throw std::runtime_error("probe short read");return out;
}
int main(){auto a=render(false),b=render(true);double energy=0;
 for(size_t i=0;i<a.size();i++){if(!std::isfinite(b[i])||std::abs(a[i]-b[i])>2e-5)return 2;energy+=std::abs(b[i]);}
 if(energy<1)return 3;std::cout<<"DUCKING_API_NORMAL_OK\n";
}
