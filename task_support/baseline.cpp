#include "offline_ports.h"
#include <cstdio>
#include <cstdint>
int main(){
 auto file=fixture::spc();
 for(long rate:{32000L,44100L,48000L}){
  fixture::OfflinePorts a,b,noevents;Spc_Emu legacy;
  std::vector<fixture::PortEvent>events={{1000,0,37},{1000,1,91},{2200,0,213},{5100,1,7},{16000,0,61}};
  if(a.begin(file,rate,events)||b.begin(file,rate,events)||noevents.begin(file,rate,{})||fixture::setup(legacy,file,rate))return 1;
  long frames=rate;std::vector<short>x(frames*2),y(frames*2),z(frames*2),old(frames*2);
  if(a.read(x.data(),frames))return 2;
  for(long pos=0;pos<frames;){long n=std::min(257L,frames-pos);if(b.read(y.data()+pos*2,n))return 3;pos+=n;}
  if(noevents.read(z.data(),frames)||fixture::render(legacy,old.data(),frames,1024))return 4;
  unsigned diff=0,changed=0,old_diff=0,nonzero=0;
  for(size_t i=0;i<x.size();i++){diff+=x[i]!=y[i];changed+=x[i]!=z[i];old_diff+=z[i]!=old[i];nonzero+=x[i]!=0;}
  std::printf("SPC_PORT rate=%ld frames=%ld nonzero=%u partition_mismatches=%u old_path_mismatches=%u event_changed_samples=%u native=%llu\n",rate,frames,nonzero,diff,old_diff,changed,(unsigned long long)a.native_frame());
  if(diff||old_diff||changed<1000||nonzero<1000)return 5;
 }
 std::puts("BASELINE_COMPLETE spc_timed_ports_resampling");return 0;
}
