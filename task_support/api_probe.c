#include "reassembly.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ngtcp2_ksl.h"
#include "ngtcp2_rob.h"
#include "ngtcp2_strm.h"
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"API_PROBE_FAILED line=%d\n",__LINE__);return 71;}}while(0)
int main(void) {
  const ngtcp2_mem *mem=ngtcp2_mem_default();ngtcp2_ksl ksl;ngtcp2_ksl_init(&ksl,ngtcp2_ksl_uint64_less,ngtcp2_ksl_uint64_less_search,sizeof(uint64_t),mem);
  for(uint64_t i=0;i<100;++i)REQUIRE(ngtcp2_ksl_insert_atomic(&ksl,NULL,&i,NULL)==0);REQUIRE(ngtcp2_ksl_len(&ksl)==100);ngtcp2_ksl_free(&ksl);
  const uint8_t body[]={'a','b','c','d'};const uint8_t *out=NULL;ngtcp2_rob rob;REQUIRE(ngtcp2_rob_init(&rob,8,mem)==0);REQUIRE(ngtcp2_rob_push_atomic(&rob,2,body+2,2)==2);REQUIRE(ngtcp2_rob_push_atomic(&rob,0,body,2)==2);REQUIRE(ngtcp2_rob_data_at(&rob,&out,0)==4);REQUIRE(memcmp(out,body,4)==0);ngtcp2_rob_free(&rob);
  ngtcp2_objalloc frames;ngtcp2_objalloc_init(&frames,1024,mem);ngtcp2_strm s;ngtcp2_strm_init(&s,0,NGTCP2_STRM_FLAG_NONE,32,32,NULL,&frames,mem);s.rx.last_offset=4;
  REQUIRE(ngtcp2_strm_recv_reordering_atomic(&s,body+2,2,2)==2);REQUIRE(ngtcp2_strm_recv_reordering_atomic(&s,body,2,0)==2);REQUIRE(ngtcp2_strm_rx_offset(&s)==4);ngtcp2_strm_free(&s);ngtcp2_objalloc_free(&frames);
  const char empty[]="RXA1\n";rxcheck_result r={0};REQUIRE(rxcheck_run((const uint8_t*)empty,5,&r,NULL)==0);REQUIRE(r.operations==0 && r.accepted_bytes==0 && r.delivered_bytes==0 && r.canceled==0 && r.oom_retries==0);
  const char bodyplan[]="RXA1\n00000100\n0100100f\n0100000f\n0200ff00\n";memset(&r,0,sizeof(r));REQUIRE(rxcheck_run((const uint8_t*)bodyplan,sizeof(bodyplan)-1,&r,NULL)==0);REQUIRE(r.operations==4 && r.accepted_bytes==258 && r.delivered_bytes==258 && r.canceled==0);
  const char pressure[]="RXA1\n00000100\n04000000\n0100100f\n0100000f\n0200ff00\n";memset(&r,0,sizeof(r));REQUIRE(rxcheck_run((const uint8_t*)pressure,sizeof(pressure)-1,&r,NULL)==0);REQUIRE(r.operations==5 && r.accepted_bytes==258 && r.delivered_bytes==258 && r.oom_retries==1);
  const char cancel[]="RXA1\n00000100\n00010101\n0100100f\n03000000\n0101000f\n0201ff00\n";memset(&r,0,sizeof(r));REQUIRE(rxcheck_run((const uint8_t*)cancel,sizeof(cancel)-1,&r,NULL)==0);REQUIRE(r.operations==6 && r.accepted_bytes==274 && r.delivered_bytes==258 && r.canceled==1);
  const char invalid[]="RXA1\n05000000\n";REQUIRE(rxcheck_run((const uint8_t*)invalid,sizeof(invalid)-1,&r,NULL)==2);
  uint8_t a[1157],b[1157];memcpy(a,empty,5);memcpy(b,empty,5);size_t na=rxcheck_mutate(a,5,sizeof(a),35),nb=rxcheck_mutate(b,5,sizeof(b),35);REQUIRE(na>=5 && na<=sizeof(a) && na==nb && memcmp(a,b,na)==0);memset(&r,0,sizeof(r));REQUIRE(rxcheck_run(a,na,&r,NULL)==0);
  puts("API_PROBE_OK production=ksl+rob+strm model=body+mutator");
}
