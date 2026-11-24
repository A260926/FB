#ifndef TASK_OFFLINE_PORTS_H
#define TASK_OFFLINE_PORTS_H
#include "spc_fixture.h"
#include <cstdint>
#include <algorithm>
namespace fixture {
struct PortEvent { uint64_t frame; unsigned port,value; };
// Synchronous reference only: the complete schedule is fixed before rendering.
// No snapshots, editing, speculation, rollback, or production Preview API.
class OfflinePorts {
 Snes_Spc apu;SPC_Filter filter;Fir_Resampler<24> fir;
 std::vector<PortEvent> events;size_t next;uint64_t native;long rate;
 const char* input(short*out,long frames){
  long pos=0;
  while(pos<frames){
   while(next<events.size()&&events[next].frame==native){apu.write_port(0,events[next].port,events[next].value);next++;}
   long n=frames-pos;if(next<events.size()&&events[next].frame-native<uint64_t(n))n=long(events[next].frame-native);
   if(n<=0)return "bad offline schedule";
   const char*err=apu.play(int(n*2),out+pos*2);if(err)return err;native+=n;pos+=n;
  }
  filter.run(out,int(frames*2));return 0;
 }
public:
 OfflinePorts():next(0),native(0),rate(0){}
 const char* begin(const std::vector<unsigned char>&b,long r,std::vector<PortEvent>plan){
  rate=r;events=plan;next=0;native=0;
  std::sort(events.begin(),events.end(),[](const PortEvent&a,const PortEvent&z){return a.frame<z.frame||(a.frame==z.frame&&a.port<z.port);});
  const char*err=apu.init();if(err)return err;err=apu.load_spc(b.data(),long(b.size()));if(err)return err;apu.clear_echo();filter.clear();filter.enable(true);filter.set_gain(int(1.4*SPC_Filter::gain_unit));
  if(rate!=32000){err=fir.buffer_size(3200);if(err)return err;fir.time_ratio(32000.0/rate,0.9965);}return 0;
 }
 const char* read(short*out,long frames){
  if(rate==32000){while(frames){long n=std::min(frames,1600L);const char*err=input(out,n);if(err)return err;out+=2*n;frames-=n;}return 0;}
  long remain=frames*2;
  while(remain){int n=fir.read(out,int(remain));remain-=n;out+=n;if(remain){int size=fir.max_write();const char*err=input(fir.buffer(),size/2);if(err)return err;fir.write(size);}}
  return 0;
 }
 uint64_t native_frame()const{return native;}
};
}
#endif
