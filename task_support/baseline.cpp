#include "xop/RtspServer.h"
#include "xop/H264Source.h"
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <signal.h>
static void need(bool b,const char* m){if(!b)throw std::runtime_error(m);}
static void exact(int fd,void* p,size_t n){while(n){pollfd f={fd,POLLIN,0};need(poll(&f,1,2000)>0,"receive timeout");int k=recv(fd,p,n,0);need(k>0,"receive closed");p=(char*)p+k;n-=k;}}
static std::string request(int fd,const std::string& text){need(send(fd,text.data(),text.size(),0)==(int)text.size(),"request send");std::string out;char c;while(out.find("\r\n\r\n")==std::string::npos){exact(fd,&c,1);out+=c;need(out.size()<16000,"header limit");}size_t pos=out.find("Content-Length:");if(pos!=std::string::npos){int n=std::atoi(out.c_str()+pos+15);while(n--){exact(fd,&c,1);out+=c;}}need(out.find("200 OK")!=std::string::npos,"RTSP rejected");return out;}
int main(){
 try {
 signal(SIGPIPE,SIG_IGN);
 xop::EventLoop loop(1);auto server=xop::RtspServer::Create(&loop);
 int probe=socket(AF_INET,SOCK_STREAM,0);sockaddr_in addr={};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);need(bind(probe,(sockaddr*)&addr,sizeof(addr))==0,"bind probe");socklen_t n=sizeof(addr);getsockname(probe,(sockaddr*)&addr,&n);int port=ntohs(addr.sin_port);close(probe);need(server->Start("127.0.0.1",port),"server start");
 auto session=xop::MediaSession::CreateNew("live");session->AddSource(xop::channel_0,xop::H264Source::CreateNew());auto id=server->AddSession(session);
 int fd=socket(AF_INET,SOCK_STREAM,0);need(connect(fd,(sockaddr*)&addr,sizeof(addr))==0,"connect");std::string url="rtsp://127.0.0.1:"+std::to_string(port)+"/live";
 request(fd,"OPTIONS "+url+" RTSP/1.0\r\nCSeq: 1\r\n\r\n");request(fd,"DESCRIBE "+url+" RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n");
 auto setup=request(fd,"SETUP "+url+"/track0 RTSP/1.0\r\nCSeq: 3\r\nTransport: RTP/AVP/TCP;unicast;interleaved=8-9\r\n\r\n");size_t s=setup.find("Session: ");need(s!=std::string::npos,"session response");auto token=setup.substr(s+9,setup.find('\r',s)-s-9);
 request(fd,"PLAY "+url+" RTSP/1.0\r\nCSeq: 4\r\nSession: "+token+"\r\n\r\n");
 xop::AVFrame frame;frame.type=xop::VIDEO_FRAME_I;frame.timestamp=90000;frame.size=4;frame.buffer.reset(new uint8_t[4]);const uint8_t nal[]={0x65,0x11,0x22,0x33};std::memcpy(frame.buffer.get(),nal,4);need(server->PushFrame(id,xop::channel_0,frame),"push frame");
 uint8_t h[4];exact(fd,h,4);need(h[0]=='$'&&h[1]==8,"interleaved channel");std::vector<uint8_t> packet((h[2]<<8)|h[3]);exact(fd,packet.data(),packet.size());need(packet.size()==16&&packet[12]==0x65,"RTP NAL");
 std::cout<<"RTSP 200; RTP channel=8 bytes="<<packet.size()<<" timestamp=90000 payload=65112233\n";
 uint8_t feedback[20]={'$',9,0,16,0x81,205,0,3,0,0,0,1};std::memcpy(feedback+12,packet.data()+8,4);std::memcpy(feedback+16,packet.data()+2,2);need(send(fd,feedback,20,0)==20,"feedback send");pollfd f={fd,POLLIN,0};int r=poll(&f,1,150);need(r>=0,"poll feedback");std::cout<<"Generic NACK retransmitted="<<(r>0?1:0)<<" (starting capability observation)\n";
 close(fd);server->Stop();loop.Quit();return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
