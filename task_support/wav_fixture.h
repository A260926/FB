#ifndef TASK_WAV_FIXTURE_H
#define TASK_WAV_FIXTURE_H
#include <vector>
#include <cstdint>
#include <cstring>
namespace fixture {
inline void u16(std::vector<unsigned char>& b, unsigned v){b.push_back(v&255);b.push_back(v>>8);}
inline void u32(std::vector<unsigned char>& b, unsigned v){u16(b,v&65535);u16(b,v>>16);}
inline short value(unsigned n,unsigned c){return short(int((n*73u+n*n*19u+c*1901u)%8001u)-4000);}
inline std::vector<unsigned char> wav(unsigned frames=8192,unsigned channels=2,unsigned rate=32000){
 std::vector<unsigned char>b;const char*a="RIFF";b.insert(b.end(),a,a+4);u32(b,36+frames*channels*2);
 a="WAVEfmt ";b.insert(b.end(),a,a+8);u32(b,16);u16(b,1);u16(b,channels);u32(b,rate);u32(b,rate*channels*2);u16(b,channels*2);u16(b,16);
 a="data";b.insert(b.end(),a,a+4);u32(b,frames*channels*2);
 for(unsigned i=0;i<frames;i++)for(unsigned c=0;c<channels;c++)u16(b,(unsigned short)value(i,c));return b;
}
}
#endif
