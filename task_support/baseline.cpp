#include "Ap4.h"
#include "Ap4SyntheticSampleTable.h"
#include "Ap4FileWriter.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#define REQUIRE(x) do { if(!(x)){std::fprintf(stderr,"baseline failed at %d\n",__LINE__);std::exit(1);} }while(0)
class ForwardOnly:public AP4_ByteStream {
public:
 AP4_Position pos; unsigned seeks; ForwardOnly():pos(0),seeks(0){}
 AP4_Result ReadPartial(void* b,AP4_Size n,AP4_Size& got){static const char data[]="ABCD";got=(AP4_Size)((4-pos<n)?4-pos:n);if(!got)return AP4_ERROR_EOS;std::memcpy(b,data+pos,got);pos+=got;return AP4_SUCCESS;}
 AP4_Result WritePartial(const void*,AP4_Size,AP4_Size& n){n=0;return AP4_ERROR_NOT_SUPPORTED;}
 AP4_Result Seek(AP4_Position){++seeks;return AP4_ERROR_NOT_SUPPORTED;}
 AP4_Result Tell(AP4_Position& p){p=pos;return AP4_SUCCESS;}
 AP4_Result GetSize(AP4_LargeSize& s){s=4;return AP4_SUCCESS;}
 void AddReference(){} void Release(){}
};
int main(int argc,char** argv){
 const AP4_UI08 raw[]={'A','B','C','D','E','F','G','H','I','J','K','L'};
 AP4_MemoryByteStream* source=new AP4_MemoryByteStream(raw,sizeof(raw));
 AP4_Movie* movie=new AP4_Movie(1000,30);
 AP4_SyntheticSampleTable* table=new AP4_SyntheticSampleTable(1);
 table->AddSampleDescription(new AP4_SampleDescription(AP4_SampleDescription::TYPE_UNKNOWN,AP4_ATOM_TYPE('t','s','t','1'),NULL));
 REQUIRE(table->AddSample(*source,8,4,10,0,0,0,true)==AP4_SUCCESS);
 REQUIRE(table->AddSample(*source,0,4,10,0,10,0,true)==AP4_SUCCESS);
 REQUIRE(table->AddSample(*source,4,4,10,0,20,0,true)==AP4_SUCCESS);
 AP4_Track* track=new AP4_Track(AP4_Track::TYPE_VIDEO,table,1,1000,30,1000,30,"und",16<<16,16<<16);
 REQUIRE(movie->AddTrack(track)==AP4_SUCCESS);
 AP4_File file(movie);
 AP4_MemoryByteStream* encoded=new AP4_MemoryByteStream();
 REQUIRE(AP4_FileWriter::Write(file,*encoded)==AP4_SUCCESS);
 REQUIRE(encoded->Seek(0)==AP4_SUCCESS);
 {
  AP4_File decoded(*encoded);AP4_Movie* m=decoded.GetMovie();REQUIRE(m!=NULL);
  AP4_Track* t=m->GetTrack((AP4_UI32)1);REQUIRE(t && t->GetSampleCount()==3);
  const char* expected[]={"IJKL","ABCD","EFGH"};
  for(unsigned i=0;i<3;++i){AP4_Sample s;AP4_DataBuffer b;REQUIRE(t->ReadSample(i,s,b)==AP4_SUCCESS);REQUIRE(b.GetDataSize()==4 && !std::memcmp(b.GetData(),expected[i],4));REQUIRE(s.GetDts()==i*10);std::printf("ROUNDTRIP sample=%u dts=%llu bytes=%.4s\n",i,(unsigned long long)s.GetDts(),b.GetData());}
 }
 if(argc<2 || std::strcmp(argv[1],"--normal-only")){
  ForwardOnly forward;AP4_Sample sample(forward,0,4,10,0,0,0,true);AP4_DataBuffer b;
  REQUIRE(sample.ReadData(b)==AP4_ERROR_NOT_SUPPORTED);REQUIRE(forward.seeks==1 && forward.pos==0);
  std::puts("RAW_SAMPLE_FORWARD_SEEK_UNSUPPORTED");
 }
 encoded->Release();source->Release();std::puts("BASELINE_REAL_FILE_WRITER_AND_SAMPLE_READ_OK");return 0;
}
