#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"
#include "mkvparser/mkvparser.h"
#include "mkvparser/mkvreader.h"
#include <cstdio>
#include <filesystem>
#include <string>
namespace fs = std::filesystem;
static bool write_file(const std::string& path, const std::string& chunks) {
  mkvmuxer::MkvWriter writer;
  if (!writer.Open(path.c_str())) return false;
  mkvmuxer::Segment s;
  if (!s.Init(&writer)) return false;
  s.GetSegmentInfo()->set_writing_app("standalone-baseline");
  if (!chunks.empty() && !s.SetChunking(true, chunks.c_str())) return false;
  if (s.AddVideoTrack(16,16,1)!=1 || s.AddAudioTrack(48000,2,2)!=2) return false;
  s.GetTrackByNumber(1)->set_uid(101);
  s.GetTrackByNumber(2)->set_uid(102);
  s.GetTrackByNumber(1)->set_codec_id("V_VP8");
  auto* audio=s.GetTrackByNumber(2); audio->set_codec_id("A_OPUS");
  const uint8_t head[]={ 'O','p','u','s','H','e','a','d',1,2,0,0,0x80,0xbb,0,0,0,0,0 };
  if (!audio->SetCodecPrivate(head,sizeof(head))) return false;
  s.set_max_cluster_duration(40000000);
  struct Item { int track,ms; bool key; };
  const Item items[]={{1,0,true},{2,0,true},{2,20,true},{2,40,true},
                       {1,40,false},{2,60,true},{2,80,true},{1,80,true},
                       {2,100,true},{1,120,false},{2,120,true}};
  unsigned serial=0;
  for (auto i:items) {
    // These are distinguishable compressed records for container checks, not decodable audio/video.
    const uint8_t data[]={uint8_t(i.track),uint8_t(i.ms),uint8_t(++serial),0x55};
    mkvmuxer::Frame f;
    if (!f.Init(data,sizeof(data))) return false;
    f.set_track_number(i.track); f.set_timestamp(uint64_t(i.ms)*1000000);
    f.set_is_key(i.key); f.set_duration((i.track==1?40ULL:20ULL)*1000000);
    if (!s.AddGenericFrame(&f)) return false;
  }
  return s.Finalize();
}
static int count_frames(const std::string& path, long long& duration) {
  mkvparser::MkvReader r; if (r.Open(path.c_str())) return -1;
  mkvparser::EBMLHeader h; long long pos=0; if(h.Parse(&r,pos)) return -2;
  mkvparser::Segment* raw=nullptr;
  if(mkvparser::Segment::CreateInstance(&r,pos,raw) || !raw) return -3;
  std::unique_ptr<mkvparser::Segment> s(raw);
  if(s->Load()) return -4;
  duration=s->GetInfo()->GetDuration(); int n=0;
  for(auto* c=s->GetFirst();c&&!c->EOS();c=s->GetNext(c)) {
    const mkvparser::BlockEntry* e=nullptr; if(c->GetFirst(e)) return -5;
    while(e&&!e->EOS()) {
      const auto* b=e->GetBlock();
      for(int i=0;i<b->GetFrameCount();++i) {
        const auto& f=b->GetFrame(i); uint8_t data[4];
        if(f.len!=4 || f.Read(&r,data) || (data[0]!=1 && data[0]!=2) || data[3]!=0x55) return -6;
        ++n;
      }
      if(c->GetNext(e,e)) return -7;
    }
  }
  return n;
}
int main() {
  fs::path dir=".task-build/baseline-output";
  fs::remove_all(dir); fs::create_directories(dir);
  if(!write_file((dir/"ordinary.webm").string(),"") ||
     !write_file((dir/"unused.webm").string(),(dir/"recording").string())) return 1;
  long long duration=0; int n=count_frames((dir/"ordinary.webm").string(),duration);
  if(n!=11) return 2;
  std::printf("ordinary_roundtrip frames=%d duration_ns=%lld\n",n,duration);
  int chunks=0,standalone=0;
  for(const auto& e:fs::directory_iterator(dir)) if(e.path().extension()==".chk") {
    ++chunks; long long d=0; int result=count_frames(e.path().string(),d);
    if(result>=0) ++standalone;
  }
  if(chunks<2 || standalone!=0 || !fs::exists(dir/"recording.hdr")) return 3;
  std::printf("shared_header_chunks=%d independently_parseable=%d\n",chunks,standalone);
  std::puts("BASELINE_OK current_chunk_files_need_shared_header");
}
