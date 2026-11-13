#include "intel-pt.h"
#include "pt_time.h"
#include <stdio.h>
#include <stdlib.h>
int main(void){
 struct pt_config c;struct pt_packet p;struct pt_time t;unsigned char buf[128];uint64_t len,tsc;uint32_t lm,lc;int n,count=0;
 pt_config_init(&c);c.begin=buf;c.end=buf+sizeof(buf);
 struct pt_encoder *e=pt_alloc_encoder(&c);if(!e)return 1;
 memset(&p,0,sizeof(p));p.type=ppt_psb;if(pt_enc_next(e,&p)<0)return 2;
 p.type=ppt_tsc;p.payload.tsc.tsc=1000;if(pt_enc_next(e,&p)<0)return 3;
 p.type=ppt_cyc;p.payload.cyc.value=10;if(pt_enc_next(e,&p)<0)return 4;
 p.type=ppt_psbend;if(pt_enc_next(e,&p)<0)return 5;
 if(pt_enc_get_offset(e,&len)<0)return 6;pt_free_encoder(e);c.end=buf+len;
 struct pt_packet_decoder *d=pt_pkt_alloc_decoder(&c);if(!d)return 7;
 if(pt_pkt_sync_forward(d)<0)return 8;pt_time_init(&t);
 while((n=pt_pkt_next(d,&p,sizeof(p)))>0){
  count++;
  if(p.type==ppt_tsc && pt_time_update_tsc(&t,&p.payload.tsc,&c)<0)return 9;
  if(p.type==ppt_cyc && pt_time_update_cyc(&t,&p.payload.cyc,&c,1ull<<8)<0)return 10;
 }
 pt_pkt_free_decoder(d);
 if(n!=-pte_eos||count!=4||pt_time_query_tsc(&tsc,&lm,&lc,&t)<0||tsc!=1010||lm||lc)return 11;
 printf("PT_PACKETS count=%d time=%llu lost_mtc=%u lost_cyc=%u\n",count,(unsigned long long)tsc,lm,lc);
 puts("BASELINE_OK packet_decode_time_update");return 0;
}
