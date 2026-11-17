#include <pjmedia/rtp.h>
#include <pj/sock.h>
#include <pj/rand.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(void) {
 pjmedia_rtp_session s;
 pjmedia_rtp_session_setting cfg;
 const void *encoded=NULL,*payload=NULL;
 const pjmedia_rtp_hdr *hdr=NULL;
 int hlen=0;unsigned plen=0;
 unsigned char raw[16];
 memset(&cfg,0,sizeof(cfg));cfg.flags=1|2|4|8;cfg.default_pt=111;cfg.sender_ssrc=0x12345678;cfg.seq=65534;cfg.ts=0xfffffc40u;
 pj_srand(1);
 CHECK(pjmedia_rtp_session_init2(&s,cfg)==0);
 for(unsigned n=0;n<3;n++) {
  CHECK(pjmedia_rtp_encode_rtp(&s,111,n==2,4,960,&encoded,&hlen)==0);
  CHECK(hlen==12);memcpy(raw,encoded,12);raw[12]='A'+n;raw[13]=0x42;raw[14]=0x43;raw[15]=0x44;
  CHECK(pjmedia_rtp_decode_rtp(&s,raw,sizeof(raw),&hdr,&payload,&plen)==0);
  CHECK(plen==4 && ((const unsigned char*)payload)[0]=='A'+n);
  CHECK(pj_ntohs(hdr->seq)==(unsigned short)(65535+n));
  CHECK(pj_ntohl(hdr->ts)==(unsigned)(0xfffffc40u+960*(n+1)));
  CHECK(pj_ntohl(hdr->ssrc)==0x12345678u && hdr->pt==111 && hdr->m==(n==2));
  printf("RTP seq=%u ts=%u payload=%c marker=%u\n",pj_ntohs(hdr->seq),pj_ntohl(hdr->ts),((const char*)payload)[0],hdr->m);
 }
 puts("BASELINE_COMPLETE packets=3 sequence_wrap=1 timestamp_wrap=1");
 return 0;
}
