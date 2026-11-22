#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/externalReference.h>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <iostream>
namespace o=opentimelineio::OPENTIMELINEIO_VERSION_NS;
static void put(std::ostream& f,uint32_t v,int n){for(int i=0;i<n;i++)f.put(char(v>>(8*i)));}
static uint32_t get(const std::vector<unsigned char>& b,size_t p,int n){uint32_t v=0;for(int i=0;i<n;i++)v|=uint32_t(b.at(p+i))<<(8*i);return v;}
int main(int argc,char**argv){
 if(argc!=3)return 2;
 const int16_t samples[]={-8000,4000,-6000,3000,-4000,2000,-2000,1000,0,0,2000,-1000,4000,-2000,6000,-3000};
 if(std::string(argv[1])=="make"){
  std::string root=argv[2];std::ofstream w(root+"/source.wav",std::ios::binary);
  w.write("RIFF",4);put(w,68,4);w.write("WAVEfmt ",8);put(w,16,4);put(w,1,2);put(w,2,2);put(w,48000,4);put(w,192000,4);put(w,4,2);put(w,16,2);
  w.write("data",4);put(w,32,4);for(auto v:samples)put(w,uint16_t(v),2);w.close();if(!w)return 3;
  o::ErrorStatus e;o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("fixture"));
  auto tr=new o::Track("audio",std::nullopt,o::Track::Kind::audio);
  o::TimeRange range(o::RationalTime(0,48000),o::RationalTime(8,48000));
  tr->append_child(new o::Clip("pcm",new o::ExternalReference("source.wav",range),range),&e);t->tracks()->append_child(tr,&e);
  auto json=t->to_json_string(&e);if(o::is_error(e))return 4;std::ofstream out(root+"/example-input.otio");out<<json;return out?0:5;
 }
 if(std::string(argv[1])!="check")return 2;
 try{
  std::ifstream f(argv[2],std::ios::binary);std::vector<unsigned char>b((std::istreambuf_iterator<char>(f)),{});
  if(b.size()<12||std::memcmp(b.data(),"RIFF",4)||std::memcmp(b.data()+8,"WAVE",4))return 6;
  size_t data=0,size=0;bool fmt=false;
  for(size_t p=12;p+8<=b.size();){auto n=get(b,p+4,4);if(n>b.size()-p-8)return 7;
   if(!std::memcmp(b.data()+p,"fmt ",4)){if(n<16||get(b,p+8,2)!=1||get(b,p+10,2)!=2||get(b,p+12,4)!=48000||get(b,p+22,2)!=16)return 8;fmt=true;}
   if(!std::memcmp(b.data()+p,"data",4)){data=p+8;size=n;}p+=8+n+(n&1);
  }
  if(!fmt||!data||size!=32)return 9;
  for(int i=0;i<16;i++)if(int16_t(get(b,data+2*i,2))!=samples[i])return 10;
  std::cout<<"AUDIO_EXAMPLE_PCM_OK frames=8 stereo=1\n";return 0;
 }catch(...){return 11;}
}
