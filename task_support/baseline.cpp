#include "MPEG2TransportStreamMultiplexor.hh"
#include "BasicUsageEnvironment.hh"
#include <vector>
#include <cstdio>
#include <cstdint>
#include <iostream>
struct Source: MPEG2TransportStreamMultiplexor {
 std::vector<unsigned char> pes;unsigned delivered=0;
 Source(UsageEnvironment& e):MPEG2TransportStreamMultiplexor(e),pes(400,0x5a){pes[0]=0;pes[1]=0;pes[2]=1;pes[3]=0xe0;pes[4]=1;pes[5]=0x8a;pes[6]=0x80;pes[7]=0;pes[8]=0;}
 void awaitNewBuffer(unsigned char*) override {
  if(delivered==3){FramedSource::handleClosure(this);return;}
  MPEG1or2Demux::SCR s;s.highBit=0;s.remainingBits=90000+delivered*90000;s.extension=0;s.isValid=True;++delivered;
  handleNewBuffer(pes.data(),pes.size(),5,s);
 }
};
struct Reader {
 Source* source;EventLoopWatchVariable done{0};unsigned char packet[188];
 unsigned pat=0,pmt=0,data=0,pcr=0;std::vector<unsigned char> rebuilt;bool okay=true;
 void next(){source->getNextFrame(packet,sizeof(packet),got,this,closed,this);}
 static void closed(void* p){((Reader*)p)->done=1;}
 static void got(void* p,unsigned n,unsigned trunc,timeval,unsigned){Reader& r=*(Reader*)p;
  if(n!=188||trunc||r.packet[0]!=0x47){r.okay=false;r.done=1;return;}
  unsigned pid=((r.packet[1]&31)<<8)|r.packet[2];
  if(pid==0)++r.pat;else if(pid==0x1000)++r.pmt;else if(pid==0xe0){
   ++r.data;unsigned offset=4;
   if(r.packet[3]&0x20){if(r.packet[4]&& (r.packet[5]&0x10))++r.pcr;offset+=1+r.packet[4];}
   if((r.packet[3]&0x10)&&offset<=188)r.rebuilt.insert(r.rebuilt.end(),r.packet+offset,r.packet+188);
  }
  r.next();
 }
};
int main(){TaskScheduler* s=BasicTaskScheduler::createNew();UsageEnvironment* e=BasicUsageEnvironment::createNew(*s);
 Source* source=new Source(*e);Reader r;r.source=source;r.next();s->doEventLoop(&r.done);
 std::vector<unsigned char> expected;for(int i=0;i<3;++i)expected.insert(expected.end(),source->pes.begin(),source->pes.end());
 bool valid=r.okay&&r.pat&&r.pmt&&r.pcr==3&&r.rebuilt==expected;
 if(valid)std::cout<<"TS_PRODUCTION pes=3 payload_bytes=1200 pcr=3 pat=present pmt=present\n";
 Medium::close(source);e->reclaim();delete s;return valid?0:1;
}
