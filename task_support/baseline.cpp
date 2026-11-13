#include "httplib.h"
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
int main(){
 int fd=socket(AF_INET,SOCK_STREAM,0); sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;
 if(fd<0||bind(fd,(sockaddr*)&a,sizeof a)||listen(fd,1))return 2;
 socklen_t z=sizeof a;if(getsockname(fd,(sockaddr*)&a,&z))return 3;
 std::thread server([&]{int c=accept(fd,nullptr,nullptr);char b[4096];if(c>=0){recv(c,b,sizeof b,0);std::string w="HTTP/1.1 103 Early Hints\r\nLink: </a.json>; rel=preload; as=fetch\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";size_t n=0;while(n<w.size()){auto k=send(c,w.data()+n,w.size()-n,0);if(k<=0)break;n+=k;}close(c);}close(fd);});
 httplib::Client cli("127.0.0.1",ntohs(a.sin_port));cli.set_read_timeout(3,0);auto r=cli.Get("/catalog");server.join();
 if(!r||r->status!=200||r->body!="ok")return 4;
 std::cout<<"status="<<r->status<<" body="<<r->body<<" final-link-count="<<r->get_header_value_count("Link")<<"\n";
 return 0;
}
