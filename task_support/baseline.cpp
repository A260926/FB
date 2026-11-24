#include "Ap4.h"
#include "Ap4Mpeg2Ts.h"
#include "ts_check.h"
#include <cstdio>
int main(int argc,char** argv){
 if(argc<2)return 1;
 AP4_ByteStream* input=nullptr;
 if(AP4_FAILED(AP4_FileByteStream::Create(argv[1],AP4_FileByteStream::STREAM_MODE_READ,input)))return 2;
 int rc=0;
 {
 AP4_File f(*input);AP4_Movie* m=f.GetMovie();if(!m)return 3;
 AP4_Track* tr=m->GetTrack(AP4_Track::TYPE_VIDEO);if(!tr||tr->GetSampleCount()!=4)return 4;
 AP4_Mpeg2TsWriter w;AP4_Mpeg2TsWriter::SampleStream* stream=nullptr;
 if(AP4_FAILED(w.SetVideoStream(1000,AP4_MPEG2_STREAM_TYPE_AVC,0xe0,stream,0x102,nullptr,0,9000)))return 5;
 AP4_MemoryByteStream* out=new AP4_MemoryByteStream();
 if(AP4_FAILED(w.WritePAT(*out))||AP4_FAILED(w.WritePMT(*out)))return 6;
 for(unsigned i=0;i<4;++i){AP4_Sample sample;AP4_DataBuffer data;if(AP4_FAILED(tr->ReadSample(i,sample,data)))return 7;
 if(AP4_FAILED(stream->WriteSample(sample,data,tr->GetSampleDescription(sample.GetDescriptionIndex()),true,*out)))return 8;}
 rc=inspect_simple(out->GetData(),out->GetDataSize(),false);
 if(rc){std::fprintf(stderr,"legacy inspector=%d\n",rc);return 9;}
 std::printf("LEGACY_TS pes=4 pts=9000,12600,16200,19800 bytes=%u\n",out->GetDataSize());out->Release();
 if(argc==2){
 AP4_Mpeg2TsWriter::Stream raw(0x102);out=new AP4_MemoryByteStream();unsigned zero=0;raw.WritePacketHeader(false,zero,true,300000,*out);
 if(out->GetDataSize()!=188||((out->GetData()[3]>>4)&3)!=3)return 10;
 std::puts("GAP_CONFIRMED zero-payload header uses AFC=3 and advances payload CC");
 out->Release();
 }
 std::puts("BASELINE_OK actual AVC sample-to-PES-to-TS chain");
 }
 input->Release();return rc;
}
