#include "rtpretransmit.h"
#include <cstdint>
int main(){
 RTPRepairConfig c={4,1024,8000,0,1000,200000,10000,20000};
 RTPRepairHistory h;
 if(h.Configure(c)!=0) return 20;
 if(h.PacketCount()!=0 || h.ByteCount()!=0) return 21;
 h.Clear();
 if(h.RememberSent(nullptr,0,0)>=0) return 22;
 return 0;
}
