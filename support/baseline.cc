#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <cstring>
#include "mkvmuxer/mkvmuxer.h"
class MemoryWriter : public mkvmuxer::IMkvWriter {
 public:
 std::vector<uint8_t> bytes; int64_t pos=0;
 mkvmuxer::int32 Write(const void* p,mkvmuxer::uint32 n) override {if(pos+n>static_cast<int64_t>(bytes.size()))bytes.resize(pos+n);std::memcpy(bytes.data()+pos,p,n);pos+=n;return 0;}
 mkvmuxer::int64 Position() const override{return pos;}
 mkvmuxer::int32 Position(mkvmuxer::int64 p) override{if(p<0)return -1;pos=p;return 0;}
 bool Seekable() const override{return true;}
 void ElementStartNotify(mkvmuxer::uint64,mkvmuxer::int64) override{}
};
static void check(bool b){if(!b)std::abort();}
int main(){
 MemoryWriter w;mkvmuxer::Segment s;check(s.Init(&w));s.set_mode(mkvmuxer::Segment::kFile);auto t=s.AddVideoTrack(16,16,1);check(t==1);
 auto *v=s.GetTrackByNumber(t);v->set_codec_id("V_MPEG4/ISO/AVC");const uint8_t cfg[]={1,66,0,10,255,225,0,1,103,1,0,1,104};check(v->SetCodecPrivate(cfg,sizeof(cfg)));
 uint8_t data[]={0,0,0,1,0x65};mkvmuxer::Frame f;check(f.Init(data,sizeof(data)));f.set_track_number(t);f.set_is_key(true);f.set_duration(40000000);f.set_timestamp(80000000);check(s.AddGenericFrame(&f));
 f.set_timestamp(160000000);f.set_is_key(false);f.set_reference_block_timestamp(80000000);check(s.AddGenericFrame(&f));
 f.set_timestamp(120000000);bool reordered=s.AddGenericFrame(&f);check(!reordered);bool done=s.Finalize();check(done&&w.bytes.size()>100);
 std::printf("baseline: forward=2 reordered=%d finalized=%d\n",reordered,done);
}
