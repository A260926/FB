#include "Spc_Preview.h"
#include "offline_ports.h"
#include <vector>
#include <cstdio>
int main(){
 auto b=fixture::spc(3);Spc_Preview p;fixture::OfflinePorts reference;
 if(p.begin(b.data(),long(b.size()),44100,2048)||p.schedule_port(0,0,43)||reference.begin(b,44100,{{0,0,43}}))return 1;
 auto s=p.status();if(s.delivered_frames||s.committed_native_frame)return 2;
 if(p.prefetch())return 3;s=p.status();if(s.pending_frames!=2048||s.delivered_frames||s.committed_native_frame||!s.synthesized_native_frames)return 4;
 auto work=s.synthesized_native_frames;if(p.prefetch()||p.status().synthesized_native_frames!=work)return 5;
 std::vector<short>a(514),r(514);if(p.read(a.data(),257)||reference.read(r.data(),257)||a!=r||p.status().delivered_frames!=257)return 6;
 if(p.read(nullptr,0)||p.status().delivered_frames!=257)return 7;p.reset();if(p.status().delivered_frames||p.status().synthesized_native_frames||p.status().pending_frames)return 8;
 std::puts("API_CONSUMER_COMPLETE spc_ports PCM=matched prefetch=idempotent");return 0;
}
