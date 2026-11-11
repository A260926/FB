extern "C" {
#include "frei0r.h"
}
#include <cassert>
#include <cstdio>
int main() {
 assert(f0r_init()); auto s=f0r_construct(2,1); assert(s);
 double delay=0.4; f0r_set_param_value(s,&delay,0);
 const double times[]={0,0.3,0.8}; const uint32_t values[]={11,33,88}; uint32_t in[2],out[2];
 for(int i=0;i<3;i++) { in[0]=in[1]=values[i]; f0r_update(s,times[i],in,out); }
 std::printf("delay t=0.8 target=0.4 observed=%u desired=33\n",out[0]);
 assert(out[0]==33); f0r_destruct(s);f0r_deinit();
 std::puts("delay0r timestamp selection behavior: correct");
}
