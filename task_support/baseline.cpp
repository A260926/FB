#include "RtpQueue.h"
#include "ScreamTx.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
extern int task_packet_freed;
void packet_free(void *,uint32_t);
#define released task_packet_freed
static bool enqueue(RtpQueue &q,unsigned short seq,bool marker,float at,uint32_t stamp) {
 void *p=std::malloc(100); if(!p)return false;
 if(!q.push(p,100,42,seq,marker,at,stamp)){std::free(p);return false;}return true;
}
int main() {
 RtpQueue q; ScreamV2Tx tx;tx.enablePacketPacing(false);
 tx.registerNewStream(&q,42,1.0f,100000,500000,1000000,10.0f);
 uint32_t empty=0; if(tx.isOkToTransmit(1,empty)!=-1.0f)return 1;
 if(!enqueue(q,10,false,1.0f,90000))return 1;
 tx.newMediaFrame(65536,42,100,false);
 uint32_t selected=0; if(tx.isOkToTransmit(65536,selected)!=0||selected!=42)return 2;
 void *p=nullptr;int size=0;uint32_t ssrc=0,stamp=0;unsigned short seq=0;bool marker=false;
 if(!q.pop(&p,size,ssrc,seq,marker,stamp)||seq!=10)return 3;
 tx.addTransmitted(65536,42,size,seq,marker,0,stamp);packet_free(p,ssrc);
 if(!enqueue(q,11,true,1.20f,90000)||!enqueue(q,12,true,1.24f,93000))return 4;
 float delay=q.getDelay(1.25f);
 if(std::fabs(delay-0.05f)>0.0001f||q.sizeOfQueue()!=2||q.bytesInQueue()!=200)return 5;
 selected=0; float allowed=tx.isOkToTransmit(81920,selected);
 if(allowed!=0||selected!=42)return 6;
 std::printf("BASELINE_OK head_seq=%d packets=%d bytes=%d head_delay=%.2f frame_age=0.25 selected=%u\n",q.seqNrOfNextRtp(),q.sizeOfQueue(),q.bytesInQueue(),delay,selected);
 q.clear();if(released!=3||q.bytesInQueue()!=0||q.sizeOfQueue()!=0)return 7;
 return 0;
}
