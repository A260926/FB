#define main legacy_resampler_baseline
#include "task_support/legacy_baseline.cpp"
#undef main
#include "task_support/position_source.h"
int main(){
 if(int code=legacy_resampler_baseline())return code;
 juce::TimeSliceThread worker("baseline-prefetch");
 TaskPositionSource sa(1),sb(2);
 juce::BufferingAudioSource a(&sa,worker,false,4096,2,false),b(&sb,worker,false,4096,2,false);
 a.prepareToPlay(64,48000);b.prepareToPlay(64,48000);
 if(!worker.startThread())return 20;
 juce::AudioBuffer<float> ya(2,64),yb(2,64);juce::AudioSourceChannelInfo ia(&ya,0,64),ib(&yb,0,64);
 if(!a.waitForNextAudioBlockReady(ia,4000)||!b.waitForNextAudioBlockReady(ib,4000))return 21;
 a.getNextAudioBlock(ia);b.getNextAudioBlock(ib);
 for(int c=0;c<2;++c)for(int n=0;n<64;++n){
  if(std::fabs(ya.getSample(c,n)-TaskPositionSource::sample(n,c,1))>1e-7)return 22;
  if(std::fabs(yb.getSample(c,n)-TaskPositionSource::sample(n,c,2))>1e-7)return 23;
 }
 if(!a.waitForNextAudioBlockReady(ia,4000)||!b.waitForNextAudioBlockReady(ib,4000))return 24;
 if(!worker.stopThread(4000))return 25;
 const auto beforeA=a.getNextReadPosition();b.setNextReadPosition(12000);
 a.getNextAudioBlock(ia);b.getNextAudioBlock(ib);
 double energyB=0;for(int c=0;c<2;++c)for(int n=0;n<64;++n)energyB+=std::fabs(yb.getSample(c,n));
 std::printf("LEGACY_SPLIT_OBSERVATION ready_before_seek=2 a_before=%lld a_after=%lld b_after=%lld b_abs=%.6f\n",(long long)beforeA,(long long)a.getNextReadPosition(),(long long)b.getNextReadPosition(),energyB);
 a.releaseResources();b.releaseResources();
 std::printf("JUCE_BUFFER_BASELINE_OK tracks=2 pcm_checks=256 normal_frames=64\n");return 0;
}
