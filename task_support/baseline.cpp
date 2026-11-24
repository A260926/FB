#include <mp4v2/mp4v2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc,char** argv){
 if(argc!=2 && argc!=3)return 2;
 MP4FileHandle f=MP4Read(argv[1]);if(f==MP4_INVALID_FILE_HANDLE)return 3;
 if(MP4GetNumberOfTracks(f)!=2||MP4GetTrackTimeScale(f,1)!=1000||MP4GetTrackNumberOfSamples(f,1)!=4)return 4;
 uint8_t* p=nullptr;uint32_t n=0;MP4Timestamp t=0;MP4Duration d=0,c=0;bool sync=false;
 if(!MP4ReadSample(f,1,3,&p,&n,&t,&d,&c,&sync)||n!=4||std::memcmp(p,"V003",4)||t!=80||d!=40)return 5;
 std::free(p);p=nullptr;n=0;
 std::puts("VIDEO sample=3 dts=80 duration=40 payload=V003");
 if(argc==3){MP4Close(f);std::puts("BASELINE_NORMAL_OK video sample tables and payload read");return 0;}
 bool tcReadable=MP4ReadSample(f,2,1,&p,&n,&t,&d,&c,&sync);
 std::free(p);MP4Close(f);
 if(tcReadable)return 6;
 std::puts("GAP_CONFIRMED tmcd ReadSample rejected: missing typed sample description");
 std::puts("BASELINE_OK normal video read and fixed-source gap asserted");
}
