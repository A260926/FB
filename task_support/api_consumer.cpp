#include "soloud.h"
#include "soloud_wavstream.h"
#include "wav_fixture.h"
#include <vector>
#include <cmath>
#include <cstdio>
int main(){
 auto bytes=fixture::wav(8192,2,48000);SoLoud::Soloud s;SoLoud::WavStream w;
 if(s.init(0,SoLoud::Soloud::NULLDRIVER,48000,512,2)||w.loadMem(bytes.data(),bytes.size(),true,false))return 1;
 s.setPostClipScaler(1);s.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);auto h=s.play(w,1,0,true);s.setPanAbsolute(h,1,1);s.setAutoStop(h,false);s.setVolume(h,1);s.setPause(h,false);
 if(s.setWavStreamDirection(h,+1)!=SoLoud::SO_NO_ERROR)return 2;
 if(s.setWavStreamDirection(h,0)!=SoLoud::INVALID_PARAMETER)return 3;
 std::vector<float>b(2048);s.mix(b.data(),1024);
 for(unsigned i=512;i<1024;i++)for(unsigned c=0;c<2;c++){
  unsigned p=unsigned((uint64_t(i)*((uint64_t(48000)<<20)/48000))>>20);
  if(std::fabs(b[2*i+c]-fixture::value(p,c)/32768.f)>1e-5)return 4;
 }
 std::puts("API_CONSUMER_COMPLETE direction=+1 PCM=matched invalid=preserved");return 0;
}
