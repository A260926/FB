#include "sequence/Sequence.h"
#include "sequence/SequenceEntry.h"
#include "probe_sound.h"
#include <cmath>
#include <iostream>
int main(){
 aud::Sequence s({8000,aud::CHANNELS_STEREO},25,false);
 auto a=s.add(std::make_shared<ProbeSound>(.125f),0,.25,0);
 auto b=s.add(std::make_shared<ProbeSound>(.25f),.032,.25,0);
 auto r=s.createQualityReader(aud::ResampleQuality::FASTEST);
 float out[1024*2];int n=1024;bool eos=false;r->read(n,eos,out);
 double energy=0;for(int i=0;i<n*2;i++){if(!std::isfinite(out[i]))return 2;energy+=std::abs(out[i]);}
 if(n!=1024||r->getPosition()!=1024||energy<1)return 3;
 b->move(.016,.25,0);r->seek(64);n=31;r->read(n,eos,out);
 if(n!=31||r->getPosition()!=95)return 4;
 std::cout<<"SEQUENCE_BASELINE_OK samples=1024 energy="<<energy<<" moved_seek=95\n";
}
