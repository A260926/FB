#ifndef TASK_MIDI_FIXTURE_HPP
#define TASK_MIDI_FIXTURE_HPP
#include <adlmidi.h>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <algorithm>
namespace task_fixture {
typedef std::vector<unsigned char> Bytes;
inline void be(Bytes &b,unsigned n,unsigned size){for(unsigned i=size;i>0;--i)b.push_back((n>>(8*(i-1)))&255);}
inline void tag(Bytes &b,const char *s){for(unsigned i=0;i<4;++i)b.push_back((unsigned char)s[i]);}
inline void vlq(Bytes &b,unsigned value){unsigned char a[4];unsigned n=0;a[n++]=value&127;while((value>>=7)!=0)a[n++]=(value&127)|128;while(n)b.push_back(a[--n]);}
inline void tempo(Bytes &b,unsigned delta,unsigned value){vlq(b,delta);b.push_back(255);b.push_back(81);b.push_back(3);be(b,value,3);}
inline void event(Bytes &b,unsigned delta,unsigned status,unsigned a,unsigned c){vlq(b,delta);b.push_back(status);b.push_back(a);b.push_back(c);}
inline void program(Bytes &b,unsigned number){b.push_back(0);b.push_back(192);b.push_back(number);}
inline void end(Bytes &b,unsigned delta){vlq(b,delta);b.push_back(255);b.push_back(47);b.push_back(0);}
inline Bytes track(unsigned index){
    Bytes b;tempo(b,0,index?250000:500000);program(b,index?16:0);
    event(b,0,176,10,index?110:20);event(b,index?240:0,144,index?72:60,100);
    event(b,480,128,index?72:60,0);end(b,index?720:480);return b;
}
inline Bytes smf(unsigned format){
    Bytes b;tag(b,"MThd");be(b,6,4);be(b,format,2);be(b,format==0?1:2,2);be(b,480,2);
    for(unsigned i=0;i<(format==0?1u:2u);++i){Bytes t=track(i);tag(b,"MTrk");be(b,(unsigned)t.size(),4);b.insert(b.end(),t.begin(),t.end());}
    return b;
}
inline ADL_MIDIPlayer *player(){
    ADL_MIDIPlayer *d=adl_init(48000);if(!d)return NULL;
    if(adl_switchEmulator(d,ADLMIDI_EMU_DOSBOX)!=0 || adl_setNumChips(d,4)<0 || adl_setBank(d,0)<0){adl_close(d);return NULL;}
    adl_setLoopEnabled(d,0);return d;
}
inline bool render(ADL_MIDIPlayer *d,unsigned frames,std::vector<short> &out){
    out.assign(frames*2,0);int n=adl_play(d,(int)out.size(),out.data());return n==(int)out.size();
}
inline bool nonzero(const std::vector<short> &p){return std::find_if(p.begin(),p.end(),[](short x){return x!=0;})!=p.end();}
}
#endif
