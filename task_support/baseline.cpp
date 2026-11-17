#include "xop/H264Source.h"
#include "xop/G711ASource.h"
#include "xop/MediaSession.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#define CHECK(x) do { if (!(x)) {std::fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main() {
 using namespace xop;
 std::unique_ptr<H264Source> video(H264Source::CreateNew());
 std::vector<RtpPacket> vp;
 video->SetSendFrameCallback([&](MediaChannelId c,RtpPacket p){if(c!=channel_0)return false;vp.push_back(p);return true;});
 AVFrame vf(3000);vf.type=VIDEO_FRAME_I;vf.timestamp=0xfffffff0u;vf.buffer.get()[0]=0x65;
 for(unsigned i=1;i<vf.size;i++)vf.buffer.get()[i]=static_cast<uint8_t>(i*17u+3u);
 CHECK(video->HandleFrame(channel_0,vf));CHECK(vp.size()==3);
 std::vector<uint8_t> rebuilt(1,0x65);
 for(size_t i=0;i<vp.size();i++){
  const RtpPacket &p=vp[i];const uint8_t *f=p.data.get()+RTP_TCP_HEAD_SIZE+RTP_HEADER_SIZE;
  CHECK(p.timestamp==vf.timestamp && p.last==(i+1==vp.size()));
  CHECK(f[0]==0x7c && (f[1]&31)==5);CHECK(!!(f[1]&128)==(i==0));CHECK(!!(f[1]&64)==(i+1==vp.size()));
  rebuilt.insert(rebuilt.end(),f+2,static_cast<const uint8_t*>(p.data.get())+p.size);
  std::printf("H264 fragment=%zu bytes=%u last=%u ts=%u\n",i,p.size,p.last,p.timestamp);
 }
 CHECK(rebuilt.size()==vf.size && std::memcmp(rebuilt.data(),vf.buffer.get(),vf.size)==0);
 std::unique_ptr<G711ASource> audio(G711ASource::CreateNew());
 AVFrame af(160);af.type=AUDIO_FRAME;af.timestamp=8000;
 for(unsigned i=0;i<af.size;i++)af.buffer.get()[i]=static_cast<uint8_t>(0x55^i);
 std::vector<RtpPacket> ap;
 audio->SetSendFrameCallback([&](MediaChannelId c,RtpPacket p){if(c!=channel_1)return false;ap.push_back(p);return true;});
 CHECK(audio->HandleFrame(channel_1,af));CHECK(ap.size()==1);
 CHECK(ap[0].size==176 && ap[0].last==1 && ap[0].timestamp==8000);
 CHECK(std::memcmp(ap[0].data.get()+16,af.buffer.get(),160)==0);
 std::printf("PCMA bytes=%u ts=%u payload=160\n",ap[0].size,ap[0].timestamp);
 std::unique_ptr<MediaSession> session(MediaSession::CreateNew("memory"));
 CHECK(session->AddSource(channel_1,G711ASource::CreateNew()));
 CHECK(session->HandleFrame(channel_1,af));
 CHECK(session->GetMediaSource(channel_1)->GetClockRate()==8000);
 std::puts("BASELINE_COMPLETE video_packets=3 audio_packets=1 session_ingress=1");
 return 0;
}
