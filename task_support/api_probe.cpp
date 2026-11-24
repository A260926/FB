#include "task_support/position_source.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdio>
static bool equal(float a,float b){return std::fabs(a-b)<1e-6;}
int main(){
 juce::TimeSliceThread worker("fixed-consumer");
 TaskPositionSource sa(1),sb(2);
 juce::BufferingAudioSource a(&sa,worker,false,4096,2,false),b(&sb,worker,false,4096,2,false);
 if(!worker.startThread())return 30;
 a.prepareToPlay(16,48000);b.prepareToPlay(16,48000);
 juce::AudioBuffer<float> pa(2,16),pb(2,16);
 juce::AudioSourceChannelInfo ia(&pa,0,16),ib(&pb,0,16);
 juce::BufferingAudioSource::ReadTicket ta,tb;
 bool peeked=false;
 for(int retry=0;retry<4000&&!peeked;++retry){
  if(!a.waitForNextAudioBlockReady(ia,1000)||!b.waitForNextAudioBlockReady(ib,1000))return 31;
  peeked=a.peekNextBlock(ia,ta)&&b.peekNextBlock(ib,tb);
  if(!peeked)juce::Thread::sleep(1);
 }
 if(!peeked||ta.getStartPosition()!=0||tb.getStartPosition()!=0||a.getNextReadPosition()!=0||b.getNextReadPosition()!=0)return 32;
 for(int c=0;c<2;++c)for(int n=0;n<16;++n){if(!equal(pa.getSample(c,n),TaskPositionSource::sample(n,c,1))||!equal(pb.getSample(c,n),TaskPositionSource::sample(n,c,2)))return 33;}
 juce::BufferingAudioSource::ReadTicket* tickets[]={&ta,&tb};
 if(!juce::BufferingAudioSource::commitReadTickets(tickets,2)||a.getNextReadPosition()!=16||b.getNextReadPosition()!=16)return 34;
 if(juce::BufferingAudioSource::commitReadTickets(tickets,2)||a.getNextReadPosition()!=16||b.getNextReadPosition()!=16)return 35;
 a.releaseResources();b.releaseResources();
 {
  juce::BufferedAudioTimeline timeline({{&a,7,1.f},{&b,13,-.5f}});
  timeline.prepareToPlay(16,48000);
  if(timeline.getOutputPosition()!=0||timeline.getTotalLength()!=19987)return 36;
  juce::AudioBuffer<float> output(2,20);output.clear();
  juce::AudioSourceChannelInfo info(&output,2,16);bool rendered=false;
  for(int retry=0;retry<4000&&!rendered;++retry){
   if(!a.waitForNextAudioBlockReady(ia,1000)||!b.waitForNextAudioBlockReady(ib,1000))return 37;
   rendered=timeline.tryRender(info);if(!rendered)juce::Thread::sleep(1);
  }
  if(!rendered||timeline.getOutputPosition()!=16)return 38;
  for(int c=0;c<2;++c)for(int n=0;n<16;++n)if(!equal(output.getSample(c,n+2),TaskPositionSource::sample(7+n,c,1)-.5f*TaskPositionSource::sample(13+n,c,2)))return 39;
  if(!timeline.seek(3)||timeline.getOutputPosition()!=3||a.getNextReadPosition()!=10||b.getNextReadPosition()!=16)return 40;
  if(!timeline.retime({11,19})||timeline.getOutputPosition()!=3||a.getNextReadPosition()!=14||b.getNextReadPosition()!=22)return 41;
  if(timeline.seek(-1)||timeline.retime({11})||a.getNextReadPosition()!=14||b.getNextReadPosition()!=22)return 42;
  timeline.releaseResources();
 }
 if(!worker.stopThread(4000))return 43;
 std::puts("ATOMIC_API_PROBE_OK pcm=64 consumed=2 seek_retime=1");return 0;
}
