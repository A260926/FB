#include "SoundTouch.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <vector>
#include <stdexcept>
using namespace soundtouch;
static void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
static std::vector<float> signal(int n,int ch) {
 std::vector<float>x(n*ch);
 for(int i=0;i<n;++i){float a=float(.25*std::sin(2*3.141592653589793*440*i/48000)+.12*std::sin(2*3.141592653589793*997*i/48000));for(int c=0;c<ch;++c)x[i*ch+c]=c?-a:a;}
 return x;
}
static std::vector<float> render(const std::vector<float>& x,int ch,int chunk,float tempo){
 SoundTouch st;st.setSampleRate(48000);st.setChannels(ch);st.setTempo(tempo);
 st.setSetting(SETTING_USE_QUICKSEEK,0);
 st.setSetting(SETTING_SEQUENCE_MS,40);st.setSetting(SETTING_SEEKWINDOW_MS,15);st.setSetting(SETTING_OVERLAP_MS,8);
 std::vector<float>out,buf(8192*ch);
 auto drain=[&](){uint n;while((n=st.receiveSamples(buf.data(),8192)))out.insert(out.end(),buf.begin(),buf.begin()+n*ch);};
 for(int pos=0;pos<int(x.size()/ch);pos+=chunk){int n=std::min(chunk,int(x.size()/ch)-pos);st.putSamples(x.data()+pos*ch,n);drain();}
 st.flush();drain();for(float v:out)require(std::isfinite(v),"nonfinite");return out;
}
static uint64_t hash(const std::vector<float>&x){uint64_t h=1469598103934665603ULL;for(float a:x){int32_t q=int32_t(std::round(a*1e6));for(int i=0;i<4;++i){h^=(uint32_t(q)>>(i*8))&255;h*=1099511628211ULL;}}return h;}
static void le(std::ostream&o,uint32_t n,int k){for(int i=0;i<k;++i)o.put(char(n>>(8*i)));}
static void writewav(const std::vector<float>&x){std::ofstream o(".build/input.wav",std::ios::binary);o.write("RIFF",4);le(o,36+x.size()*2,4);o.write("WAVEfmt ",8);le(o,16,4);le(o,1,2);le(o,2,2);le(o,48000,4);le(o,192000,4);le(o,4,2);le(o,16,2);o.write("data",4);le(o,x.size()*2,4);for(float v:x)le(o,uint16_t(int16_t(v*32767)),2);}
static void oldwav(int bits,int ch){auto x=signal(96000,ch);std::string name=".build/old-input/semmari-"+std::to_string(bits)+"b"+std::to_string(ch)+".wav";std::ofstream o(name,std::ios::binary);int width=bits/8;o.write("RIFF",4);le(o,36+x.size()*width,4);o.write("WAVEfmt ",8);le(o,16,4);le(o,1,2);le(o,ch,2);le(o,48000,4);le(o,48000*ch*width,4);le(o,ch*width,2);le(o,bits,2);o.write("data",4);le(o,x.size()*width,4);for(float v:x){uint32_t q=bits==8?uint32_t(int(v*127)+128):uint32_t(int32_t(double(v)*(double(uint64_t(1)<<(bits-1))-1)));le(o,q,width);}}
int main(){try{
 const int N=96000;
 for(int ch:{1,2})for(float tempo:{.75f,1.f,1.5f}){
  auto x=signal(N,ch);auto a=render(x,ch,4096,tempo),b=render(x,ch,37,tempo);
  require(!a.empty()&&a.size()==b.size(),"fragment duration mismatch");
  float error=0;double power=0;for(size_t i=0;i<a.size();++i){error=std::max(error,std::abs(a[i]-b[i]));power+=a[i]*a[i];if(ch==2&&i%2==0)require(std::abs(a[i]+a[i+1])<1e-6,"stereo relation");}
  require(error<1e-6&&power/a.size()>.005,"PCM baseline");
  require(std::abs(double(a.size()/ch)-N/tempo)<8192,"nominal length");
  std::printf("LEGACY_PCM channels=%d tempo=%.2f frames=%zu hash=%016llx maxdiff=%.9g\n",ch,tempo,a.size()/ch,(unsigned long long)hash(a),error);
 }
 writewav(signal(N,2));for(int bits:{8,16,24,32})for(int ch:{1,2})oldwav(bits,ch);std::puts("LEGACY_CONTROLS_OK cases=6");
}catch(const std::exception&e){std::fprintf(stderr,"baseline: %s\n",e.what());return 1;}}
