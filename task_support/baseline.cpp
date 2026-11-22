#include "SoundTouch.h"
#include <vector>
#include <cmath>
#include <cstdio>
int main(){
 for(unsigned ch: {1u,2u,6u}){
 soundtouch::SoundTouch p; p.setSampleRate(48000);p.setChannels(ch);p.setRate(1.13f);p.setTempo(.87f);
 std::vector<float> x(48000*ch), y(4096*ch);
 for(unsigned i=0;i<48000;i++)for(unsigned c=0;c<ch;c++)x[i*ch+c]=.2f*std::sin(i*.037+c*.41)+.03f*std::sin(i*i*.000013+c);
 p.putSamples(x.data(),48000); unsigned backlog=p.numSamples();
 unsigned total=0;double energy=0;auto drain=[&](){for(;;){unsigned n=p.receiveSamples(y.data(),4096);if(!n)break;total+=n;for(unsigned j=0;j<n*ch;j++){if(!std::isfinite(y[j]))return false;energy+=y[j]*y[j];}}return true;};
 if(!drain()||total<30000||energy<1)return 3;
 unsigned before=total;p.flush();if(!drain())return 2;
 std::printf("LEGACY_PCM channels=%u frames=%u queued_before_read=%u flush_added=%u energy=%.6f\n",ch,total,backlog,total-before,energy);
 }
 std::puts("BASELINE_SOUND_TOUCH_OK");
}
