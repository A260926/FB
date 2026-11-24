#ifndef ORDERED_FIXTURE_IO_H
#define ORDERED_FIXTURE_IO_H
#include <matroska/KaxSegment.h>
#include <matroska/KaxInfo.h>
#include <matroska/KaxInfoData.h>
#include <matroska/KaxChapters.h>
#include <matroska/KaxTracks.h>
#include <matroska/KaxTrackVideo.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxBlockData.h>
#include <ebml/EbmlHead.h>
#include <ebml/EbmlSubHead.h>
#include <ebml/EbmlStream.h>
#include <ebml/StdIOCallback.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <stdexcept>
#include <memory>
#include <iostream>
#include <cstring>
namespace ordered_fixture {
using namespace libebml;using namespace libmatroska;
inline std::vector<uint8_t> bytes(const char*path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error(path);return {std::istreambuf_iterator<char>(f),{}};}
inline void write(const char*path){
 auto priv=bytes("test/fixtures/codec-private.bin"),a=bytes("test/fixtures/frame0.packet"),b=bytes("test/fixtures/frame1.packet");
 StdIOCallback io(path,MODE_CREATE);EbmlHead h;GetChild<EDocType>(h).SetValue("matroska");GetChild<EDocTypeVersion>(h).SetValue(4);GetChild<EDocTypeReadVersion>(h).SetValue(2);h.Render(io);
 KaxSegment s;s.SetSizeInfinite(false);auto &info=GetChild<KaxInfo>(s);GetChild<KaxTimecodeScale>(info).SetValue(1000000);GetChild<KaxMuxingApp>(info).SetValueUTF8("fixture");GetChild<KaxWritingApp>(info).SetValueUTF8("fixture");uint8_t uid[16]={1};GetChild<KaxSegmentUID>(info).CopyBuffer(uid,16);
 auto&tr=GetChild<KaxTrackEntry>(GetChild<KaxTracks>(s));GetChild<KaxTrackNumber>(tr).SetValue(7);GetChild<KaxTrackUID>(tr).SetValue(71);GetChild<KaxTrackType>(tr).SetValue(1);GetChild<KaxCodecID>(tr).SetValue("V_THEORA");GetChild<KaxCodecPrivate>(tr).CopyBuffer(priv.data(),priv.size());GetChild<KaxTrackDefaultDuration>(tr).SetValue(1000000);tr.SetGlobalTimecodeScale(1000000);tr.EnableLacing(false);auto&v=GetChild<KaxTrackVideo>(tr);GetChild<KaxVideoPixelWidth>(v).SetValue(32);GetChild<KaxVideoPixelHeight>(v).SetValue(32);
 auto&cl=GetChild<KaxCluster>(s);cl.SetParent(s);cl.SetPreviousTimecode(0,1000000);GetChild<KaxClusterTimecode>(cl).SetValue(0);KaxBlockGroup *g0=nullptr,*g1=nullptr;
 cl.AddFrame(tr,0,*new DataBuffer(a.data(),a.size(),nullptr,true),g0,LACING_NONE);g0->SetBlockDuration(1000000);
 cl.AddFrame(tr,1000000,*new DataBuffer(b.data(),b.size(),nullptr,true),g1,LACING_NONE);GetChild<KaxReferenceBlock>(*g1).SetReferencedTimecode(-1);g1->SetBlockDuration(1000000);
 auto&e=GetChild<KaxEditionEntry>(GetChild<KaxChapters>(s));GetChild<KaxEditionUID>(e).SetValue(91);GetChild<KaxEditionFlagOrdered>(e).SetValue(1);auto&c=GetChild<KaxChapterAtom>(e);GetChild<KaxChapterUID>(c).SetValue(81);GetChild<KaxChapterTimeStart>(c).SetValue(0);GetChild<KaxChapterTimeEnd>(c).SetValue(2000000);
 s.Render(io);io.close();
}
inline int inspect(const char*path){
 StdIOCallback io(path,MODE_READ);EbmlStream stream(io);std::unique_ptr<EbmlElement> head(stream.FindNextID(EBML_INFO(EbmlHead),UINT64_MAX));if(!head||!dynamic_cast<EbmlHead*>(head.get()))return 1;head->SkipData(stream,EBML_CONTEXT(head.get()));EbmlElement* raw=stream.FindNextID(EBML_INFO(KaxSegment),UINT64_MAX);if(!raw||!dynamic_cast<KaxSegment*>(raw)){delete raw;return 1;}std::unique_ptr<KaxSegment>s(static_cast<KaxSegment*>(raw));int up=0;EbmlElement*next=nullptr;s->Read(stream,EBML_CONTEXT(s.get()),up,next,false);/* FoundElt can still alias an owned descendant when level is zero. */
 auto&cl=GetChild<KaxCluster>(*s);cl.SetParent(*s);cl.InitTimecode(GetChild<KaxClusterTimecode>(cl).GetValue(),1000000);unsigned count=0;
 for(auto*elt:cl){auto*g=dynamic_cast<KaxBlockGroup*>(elt);if(!g)continue;g->SetParent(cl);auto&block=GetChild<KaxBlock>(*g);auto expected=bytes(count?"test/fixtures/frame1.packet":"test/fixtures/frame0.packet");if(block.NumberFrames()!=1||block.GetFrameSize(0)!=(int64_t)expected.size()||memcmp(block.GetBuffer(0).Buffer(),expected.data(),expected.size())||block.GlobalTimecode()!=count*1000000)return 2;
 std::cout<<"frame="<<count<<" track="<<block.TrackNum()<<" source_ns="<<block.GlobalTimecode()<<" bytes="<<block.GetFrameSize(0)<<"\n";count++;}
 auto&c=GetChild<KaxChapterAtom>(GetChild<KaxEditionEntry>(GetChild<KaxChapters>(*s)));if(GetChild<KaxChapterTimeEnd>(c).GetValue()!=2000000||count!=2)return 3;
 std::cout<<"ordered_baseline frames="<<count<<" edition=91 chapter_end_ns=2000000\n";return 0;
}
}
#endif
