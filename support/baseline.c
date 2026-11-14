#include <ogg/ogg.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
 ogg_stream_state enc,dec; ogg_packet input={0},output;ogg_page page;
 unsigned char *bytes=malloc(70000);int pages=0,packets=0;
 assert(bytes);for(int i=0;i<70000;i++)bytes[i]=(unsigned char)(i*17+3);
 assert(ogg_stream_init(&enc,73)==0);assert(ogg_stream_init(&dec,73)==0);
 input.packet=bytes;input.bytes=70000;input.b_o_s=1;input.e_o_s=1;input.granulepos=48000;
 assert(ogg_stream_packetin(&enc,&input)==0);
 while(ogg_stream_flush(&enc,&page)){
  pages++;assert(ogg_stream_pagein(&dec,&page)==0);
  while(ogg_stream_packetout(&dec,&output)>0){
   packets++;assert(output.bytes==70000);assert(memcmp(output.packet,bytes,70000)==0);
   assert(output.granulepos==48000);assert(output.packetno==0);
  }
 }
 assert(pages==2);assert(packets==1);
 printf("OGG_BASELINE_OK pages=%d packet-bytes=70000\n",pages);
 ogg_stream_clear(&enc);ogg_stream_clear(&dec);free(bytes);return 0;
}
