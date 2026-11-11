#include "soloud.h"
#include "soloud_queue.h"
#include <cassert>
#include <cstdio>
#include <algorithm>
struct Clip : SoLoud::AudioSource {
  unsigned n; float value;
  Clip(unsigned count, float rate, float v): n(count), value(v) {mChannels=1; mBaseSamplerate=rate;}
  struct Instance : SoLoud::AudioSourceInstance {
    Clip *p; unsigned at=0; Instance(Clip *x):p(x){}
    unsigned getAudio(float *out,unsigned n,unsigned) override {
      unsigned take=std::min(n,p->n-at); std::fill(out,out+take,p->value); at+=take; return take;
    }
    bool hasEnded() override {return at==p->n;}
  };
  SoLoud::AudioSourceInstance *createInstance() override {return new Instance(this);}
};
int main() {
  SoLoud::Soloud s; assert(s.init(0,SoLoud::Soloud::NULLDRIVER,48000,512,1)==0);
  SoLoud::Queue q; assert(q.setParams(48000,1)==0); auto h=s.play(q); assert(h);
  Clip a(3,48000,.125f), b(2,48000,.25f); assert(q.play(a)==0 && q.play(b)==0);
  float out[16]={}; auto n=q.mInstance->getAudio(out,16,16);
  assert(n==5 && out[0]==.125f && out[3]==.25f && q.getQueueCount()==0);
  std::puts("QUEUE_SAME_RATE_OK frames=5"); s.stop(h);
  SoLoud::Queue mixed; mixed.setParams(48000,1); auto mh=s.play(mixed);
  Clip slow(3,24000,.125f), fast(2,48000,.25f); mixed.play(slow); mixed.play(fast);
  n=mixed.mInstance->getAudio(out,16,16); assert(n==5);
  std::puts("QUEUE_CURRENT_NO_CONVERSION frames=5 requested_rate=48000"); s.stop(mh);
}
