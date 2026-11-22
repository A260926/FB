#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>
static void require(bool x) { if (!x) throw std::runtime_error("baseline mismatch"); }
static void word(std::vector<unsigned char>& v, unsigned x) { for(int i=3;i>=0;--i)v.push_back((x>>(8*i))&255); }
struct Output { EventLoopWatchVariable done{0}; unsigned n=0; timeval time{}; unsigned char bytes[1024]{}; };
static void got(void* p,unsigned n,unsigned truncated,timeval t,unsigned) { auto& o=*(Output*)p; require(!truncated);o.n=n;o.time=t;o.done=1; }
static void timeout(void* p) { ((Output*)p)->done=2; }
static void sendPacket(int fd,unsigned seq,unsigned ts,std::vector<unsigned char> body,bool marker) {
  std::vector<unsigned char> p;word(p,0x80000000u|((marker?0x80u:0u)|96u)<<16|seq);word(p,ts);word(p,77);p.insert(p.end(),body.begin(),body.end());
  unsigned char h[4]={'$',0,(unsigned char)(p.size()>>8),(unsigned char)p.size()}; require(write(fd,h,4)==4);require(write(fd,p.data(),p.size())==(ssize_t)p.size());
}
int main() { try {
  TaskScheduler* scheduler=BasicTaskScheduler::createNew(); UsageEnvironment* env=BasicUsageEnvironment::createNew(*scheduler);
  sockaddr_storage addr{};addr.ss_family=AF_INET;
  Groupsock media(*env,addr,Port(0),0),control(*env,addr,Port(0),0);
  require(media.socketNum()>=0&&control.socketNum()>=0);
  auto* source=H264VideoRTPSource::createNew(*env,&media,96,90000);
  auto* rtcp=RTCPInstance::createNew(*env,&control,100,(unsigned char const*)"local",nullptr,source);
  int fd[2];require(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0);source->setStreamSocket(fd[1],0,nullptr);
  std::vector<unsigned char> sr;word(sr,0x80c80006);word(sr,77);word(sr,0x83aa7e80u+1000);word(sr,0);word(sr,90000);word(sr,0);word(sr,0);
  rtcp->injectReport(sr.data(),sr.size(),addr);
  Output a;source->getNextFrame(a.bytes,sizeof(a.bytes),got,&a,nullptr,nullptr);sendPacket(fd[0],10,90000,{0x65,0x11,0x22},true);
  TaskToken deadline=scheduler->scheduleDelayedTask(1000000,timeout,&a);scheduler->doEventLoop(&a.done);scheduler->unscheduleDelayedTask(deadline);
  require(a.done==1&&a.n==3&&a.bytes[0]==0x65&&a.time.tv_sec==1000&&a.time.tv_usec==0);require(source->hasBeenSynchronizedUsingRTCP());
  Output b;source->getNextFrame(b.bytes,sizeof(b.bytes),got,&b,nullptr,nullptr);sendPacket(fd[0],11,180000,{0x7c,0x85,0xaa},false);sendPacket(fd[0],12,180000,{0x7c,0x45,0xbb},true);
  deadline=scheduler->scheduleDelayedTask(1000000,timeout,&b);scheduler->doEventLoop(&b.done);scheduler->unscheduleDelayedTask(deadline);
  require(b.done==1&&b.n==3&&b.bytes[0]==0x65&&b.bytes[1]==0xaa&&b.bytes[2]==0xbb&&b.time.tv_sec==1001&&b.time.tv_usec==0);
  auto* stats=source->receptionStatsDB().lookup(77);require(stats&&stats->totNumPacketsReceived()==3&&stats->lastReceivedSR_NTPmsw()==0x83aa7e80u+1000);
  require(source->curPacketRTPSeqNum()==12&&source->curPacketMarkerBit());
  std::printf("BASELINE_RTP single=651122 fu=65aabb pts=1000,1001 packets=3 raw_sr=ok\n");
  Medium::close(rtcp);Medium::close(source);close(fd[0]);close(fd[1]);
  // Groupsock destructors still refer to the environment.
  return 0;
} catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;} }
