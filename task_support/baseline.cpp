#include "rtppacketbuilder.h"
#include "rtppacket.h"
#include "rtprawpacket.h"
#include "rtcpcompoundpacket.h"
#include "rtcppacket.h"
#include <cstring>
#include <iostream>
int main(){
 RTPPacketBuilder b;
 if(b.Init(1500)<0 || b.AddCSRC(0x12345678)<0) return 1;
 const unsigned char pcm[]={0xff,0x7f,0x01};
 unsigned char ext[]={1,2,3,4};
 uint16_t seq=b.GetSequenceNumber(); uint32_t ts=b.GetTimestamp();
 if(b.BuildPacketEx(pcm,sizeof(pcm),0,true,160,0xbeef,ext,1)<0) return 2;
 RTPTime time(1,0);
 unsigned char* raw=new unsigned char[b.GetPacketLength()];
 memcpy(raw,b.GetPacket(),b.GetPacketLength());
 RTPRawPacket wire(raw,b.GetPacketLength(),0,time,true);
 RTPPacket p(wire);
 if(p.GetCreationError()<0||p.GetSequenceNumber()!=seq||p.GetTimestamp()!=ts||p.GetCSRCCount()!=1||p.GetExtensionID()!=0xbeef||p.GetPayloadLength()!=3||memcmp(p.GetPayloadData(),pcm,3)) return 3;
 if(b.GetPacketCount()!=1||b.GetPayloadOctetCount()!=3||b.GetSequenceNumber()!=uint16_t(seq+1)||b.GetTimestamp()!=ts+160) return 4;
 unsigned char compound[]={0x80,201,0,1,0,0,0,9,0x81,205,0,3,0,0,0,9,0,0,0,7,0xff,0xff,0,3};
 unsigned char* feedback=new unsigned char[sizeof(compound)]; memcpy(feedback,compound,sizeof(compound));
 RTPRawPacket rr(feedback,sizeof(compound),0,time,false); RTCPCompoundPacket cp(rr);
 if(cp.GetCreationError()<0) return 5;
 cp.GotoFirstPacket(); RTCPPacket* first=cp.GetNextPacket(); RTCPPacket* second=cp.GetNextPacket();
 if(!first||!second||first->GetPacketType()!=RTCPPacket::RR||second->GetPacketLength()<2||second->GetPacketData()[1]!=205) return 6;
 std::cout<<"RTP_ROUNDTRIP payload=3 csrc=1 ext=4 media_packets=1\nRTCP_COMPOUND rr=1 feedback_pt=205\n";
}
