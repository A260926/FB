#include "soloud.h"
#include "soloud_wavstream.h"
#include "wav_fixture.h"
#include <cstdio>
#include <cmath>
#include <vector>
static int check(bool b,const char*m){if(!b){std::fprintf(stderr,"FAIL %s\n",m);return 1;}return 0;}
int main(){
 auto bytes=fixture::wav(8192,2,48000);SoLoud::Soloud s;SoLoud::WavStream wav;
 if(check(s.init(0,SoLoud::Soloud::NULLDRIVER,48000,512,2)==0,"null init")||check(wav.loadMem(bytes.data(),bytes.size(),true,false)==0,"WAV load"))return 1;
 s.setGlobalVolume(1);s.setPostClipScaler(1);s.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
 auto h=s.play(wav,1,0,true);s.setPanAbsolute(h,1,1);s.setVolume(h,1);s.setAutoStop(h,false);s.setVolume(h,1);s.setPause(h,false);
 std::vector<float>out(2*1024);s.mix(out.data(),1024);
 unsigned mismatch=0,nonzero=0;double peak=0;
 for(unsigned i=512;i<1024;i++)for(unsigned c=0;c<2;c++){
  unsigned pos=(unsigned)((uint64_t(i)*((uint64_t(48000)<<20)/48000))>>20);
  float wanted=fixture::value(pos,c)/32768.f;
  double delta=std::fabs(out[2*i+c]-wanted);if(delta>1e-5)mismatch++;if(out[2*i+c]!=0)nonzero++;if(delta>peak)peak=delta;
 }
 std::printf("FORWARD frames=1024 checked_samples=1024 mismatches=%u nonzero=%u max_error=%.9g\n",mismatch,nonzero,peak);
 if(check(mismatch==0&&nonzero>900,"production PCM"))return 1;
 const auto pos=s.getStreamPosition(h);const auto elapsed=s.getStreamTime(h);
 int neg=s.setRelativePlaySpeed(h,-1);s.setPause(h,true);s.mix(out.data(),7);
 if(check(neg==SoLoud::INVALID_PARAMETER,"negative speed")||check(s.getStreamPosition(h)==pos&&s.getStreamTime(h)==elapsed,"pause clocks"))return 1;
 std::printf("LEGACY negative_speed_rejected=1 pause_clocks_frozen=1 position=%.9f elapsed=%.9f\n",pos,elapsed);
 s.deinit();std::puts("BASELINE_COMPLETE soloud_wavstream");return 0;
}
