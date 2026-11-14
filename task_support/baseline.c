#include "rtmp-server.h"
#include "rtmp-chunk-header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct capture {unsigned calls;unsigned counts[64];unsigned order[16];size_t bytes;};
static int capture(void* p,const void* h,size_t hn,const void* body,size_t n){
 struct capture* c=p;const unsigned char* b=h;(void)body;
 if(hn<1||c->calls>=16)return -1;
 c->order[c->calls++]=b[0]&63;c->counts[b[0]&63]++;c->bytes+=hn+n;return (int)(hn+n);
}
int main(void){
 struct capture c={0};struct rtmp_server_handler_t handler={0};rtmp_server_t* server;unsigned char video[300],audio[2]={0xaf,1};unsigned i;
 handler.send=capture;server=rtmp_server_create(&c,&handler);if(!server)return 1;
 memset(video,0x55,sizeof(video));video[0]=0x17;
 if(rtmp_server_send_video(server,video,sizeof(video),0x01000010)!=0)return 2;
 if(c.calls!=3||c.counts[5]!=3)return 3;
 printf("video submission already emitted %u chunks; audio not submitted yet\n",c.calls);
 if(rtmp_server_send_audio(server,audio,sizeof(audio),0x01000011)!=0)return 4;
 if(c.calls!=4||c.counts[4]!=1)return 5;
 printf("wire CSID order:");for(i=0;i<c.calls;++i)printf(" %u",c.order[i]);printf("; total bytes=%zu\n",c.bytes);
 if(c.order[0]!=5||c.order[1]!=5||c.order[2]!=5||c.order[3]!=4)return 6;
 rtmp_server_destroy(server);return 0;
}
