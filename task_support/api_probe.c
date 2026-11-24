/* Fixed compiler/link consumer; tests normal empty-SEI and FLV header paths. No implementation. */
#include "caption/flv_splitter.h"
#include "caption/sei_wire.h"
#include <stdlib.h>
#include <string.h>
static const uint8_t header[13]={'F','L','V',1,5,0,0,0,9,0,0,0,0};
struct seen{unsigned count[4];};
static int emit(unsigned s,const uint8_t*p,size_t n,void *u){struct seen*v=u;if(s<1||s>4||n!=13||memcmp(p,header,13))return -1;v->count[s-1]++;return 0;}
int main(void){sei_t s;uint8_t *p=0;size_t n=0,taken=0;unsigned i;struct seen v={{0,0,0,0}};caption_flv_splitter_t*c;const uint8_t empty[]={6,0x80};
 sei_init(&s,0);if(caption_sei_decode(&s,empty,2)||s.head)return 2;
 if(caption_sei_encode(&s,&p,&n)||n!=2||memcmp(p,empty,2)){free(p);return 3;}free(p);sei_free(&s);
 c=caption_flv_splitter_new(4096,emit,&v);if(!c)return 4;
 if(caption_flv_splitter_feed(c,header,13,&taken)||taken!=13)return 5;
 if(caption_flv_splitter_finish(c)||caption_flv_splitter_finish(c))return 6;
 for(i=0;i<4;i++)if(v.count[i]!=1)return 7;
 caption_flv_splitter_free(c);return 0;}
