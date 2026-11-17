#include "ALACEncoder.h"
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void le16(FILE *f,unsigned v){std::fputc(v&255,f);std::fputc(v>>8&255,f);}
static void le32(FILE *f,unsigned v){le16(f,v&65535);le16(f,v>>16);}
static std::vector<unsigned char> readfile(const char *name) {
 FILE *f=std::fopen(name,"rb");if(!f)return {};
 std::fseek(f,0,SEEK_END);long n=std::ftell(f);std::rewind(f);
 std::vector<unsigned char> v(n);if(n && std::fread(v.data(),1,n,f)!=(size_t)n)v.clear();std::fclose(f);return v;
}
static uint32_t u32(const unsigned char *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int main() {
 const unsigned frames=4103;
 std::vector<int16_t> pcm(frames*2);
 for(unsigned i=0;i<frames;++i){pcm[i*2]=(int16_t)((i*97)%20001-10000);pcm[i*2+1]=(int16_t)((i*31)%16001-8000);}
 FILE *f=std::fopen(".task-build/normal.wav","wb");REQUIRE(f);
 std::fwrite("RIFF",1,4,f);le32(f,36+frames*4);std::fwrite("WAVEfmt ",1,8,f);le32(f,16);
 le16(f,1);le16(f,2);le32(f,48000);le32(f,192000);le16(f,4);le16(f,16);
 std::fwrite("data",1,4,f);le32(f,frames*4);for(size_t i=0;i<pcm.size();++i)le16(f,(uint16_t)pcm[i]);
 REQUIRE(std::fclose(f)==0);
 REQUIRE(std::system(".task-build/alacconvert .task-build/normal.wav .task-build/normal.caf > .task-build/encode.log 2>&1")==0);
 REQUIRE(std::system(".task-build/alacconvert .task-build/normal.caf .task-build/returned.wav > .task-build/decode.log 2>&1")==0);
 auto w=readfile(".task-build/returned.wav");REQUIRE(w.size()>=44);REQUIRE(!std::memcmp(w.data(),"RIFF",4));
 size_t pos=12,data=0,bytes=0;
 while(pos+8<=w.size()){uint32_t n=u32(w.data()+pos+4);REQUIRE(n<=w.size()-pos-8);if(!std::memcmp(w.data()+pos,"data",4)){data=pos+8;bytes=n;break;}pos+=8+n+(n&1);}
 REQUIRE(data && bytes==frames*4);
 for(size_t i=0;i<pcm.size();++i){int16_t actual=(int16_t)(w[data+2*i]|(unsigned)w[data+2*i+1]<<8);REQUIRE(actual==pcm[i]);}
 std::printf("CLI_STEREO frames=4103 channels=2 pcm_equal=1 short_tail=7\n");
 // Exercise the actual multielement codec independently of the CAF CLI.
 AudioFormatDescription input={};input.mSampleRate=48000;input.mFormatID=kALACFormatLinearPCM;
 input.mFormatFlags=kALACFormatFlagIsSignedInteger|kALACFormatFlagIsPacked;
 input.mBytesPerPacket=input.mBytesPerFrame=12;input.mFramesPerPacket=1;input.mChannelsPerFrame=6;input.mBitsPerChannel=16;
 AudioFormatDescription output={};output.mSampleRate=48000;output.mFormatID=kALACFormatAppleLossless;
 output.mFormatFlags=1;output.mChannelsPerFrame=6;output.mFramesPerPacket=4096;
 ALACEncoder encoder;encoder.SetFrameSize(4096);REQUIRE(encoder.InitializeEncoder(output)==0);
 const unsigned count=257;std::vector<int16_t> source(count*6);
 for(unsigned i=0;i<count;++i)for(unsigned c=0;c<6;++c)source[i*6+c]=(int16_t)((i*(c+11)*7+c*3023)%30001-15000);
 std::vector<unsigned char> packet(200000),cookie(encoder.GetMagicCookieSize(6));
 int32_t n=count*12;REQUIRE(encoder.Encode(input,output,(unsigned char*)source.data(),packet.data(),&n)==0);REQUIRE(n>0);
 encoder.Finish();uint32_t cs=cookie.size();encoder.GetMagicCookie(cookie.data(),&cs);REQUIRE(cs>0);
 ALACDecoder decoder;REQUIRE(decoder.Init(cookie.data(),cs)==0);
 std::vector<unsigned char> result(count*12+128);
 BitBuffer bits;BitBufferInit(&bits,packet.data(),n);uint32_t got=0;
 REQUIRE(decoder.Decode(&bits,result.data(),4096,6,&got)==0);REQUIRE(got==count);REQUIRE(!std::memcmp(result.data(),source.data(),count*12));
 std::printf("CODEC_MULTI frames=257 channels=6 packet_bytes=%d pcm_equal=1\n",n);
 return 0;
}
