#include <assert.h>
#include <math.h>
#include <stdio.h>
#define PL_MPEG_IMPLEMENTATION
#include "../pl_mpeg.h"
#include "fixture.h"
int main(void) {
 uint8_t bytes[40000];size_t n=sample_fixture(bytes);
 plm_buffer_t *b=plm_buffer_create_with_memory(bytes,n,0);
 plm_demux_t *d=plm_demux_create(b,1);assert(d);
 assert(plm_demux_has_headers(d));
 plm_packet_t *p=plm_demux_decode(d);assert(p&&p->type==0xe0&&p->length==32768);
 assert(fabs(p->pts-1.0)<1e-12);assert(p->data[3]==0&&p->data[6]==0xe0);
 for(size_t i=10;i<p->length;i++)assert(p->data[i]==(uint8_t)((i*17+0xe0)%251));
 p=plm_demux_decode(d);assert(p&&p->type==0xc0&&p->length==37);
 assert(fabs(p->pts-1.04)<1e-12);
 assert(plm_demux_decode(d)==NULL);
 plm_demux_rewind(d);p=plm_demux_decode(d);assert(p&&p->length==32768);
 puts("PS_BASELINE_OK packets=2 video=32768 audio=37 pts=1.00,1.04 rewind=ok");
 plm_demux_destroy(d);return 0;
}
