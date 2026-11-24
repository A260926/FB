#ifndef FIXTURE_IO_H
#define FIXTURE_IO_H
#include <ogg/ogg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct fixture {unsigned char *data[64];long bytes[64];ogg_int64_t gp[64];int count;};
static void fixture_free(struct fixture*f){int i;for(i=0;i<f->count;i++)free(f->data[i]);memset(f,0,sizeof(*f));}
static int fixture_read(const char*path,struct fixture*f){
 FILE*in=fopen(path,"rb");ogg_sync_state sy;ogg_stream_state st;int init=0,rc=1;ogg_page pg;ogg_packet p;
 memset(f,0,sizeof(*f));if(!in)return 1;ogg_sync_init(&sy);
 for(;;){int r=ogg_sync_pageout(&sy,&pg);if(r<0)goto done;if(!r){char*b=ogg_sync_buffer(&sy,97);size_t n=fread(b,1,97,in);if(!n){if(ferror(in))goto done;break;}ogg_sync_wrote(&sy,(long)n);continue;}
 if(!init){if(ogg_stream_init(&st,ogg_page_serialno(&pg)))goto done;init=1;}if(ogg_stream_pagein(&st,&pg))goto done;
 while((r=ogg_stream_packetout(&st,&p))>0){int i=f->count;if(i>=64)goto done;f->data[i]=malloc(p.bytes?p.bytes:1);if(!f->data[i])goto done;memcpy(f->data[i],p.packet,p.bytes);f->bytes[i]=p.bytes;f->gp[i]=p.granulepos;f->count++;}if(r<0)goto done;
 }rc=0;
 done:if(init)ogg_stream_clear(&st);ogg_sync_clear(&sy);fclose(in);if(rc)fixture_free(f);return rc;
}
#endif
