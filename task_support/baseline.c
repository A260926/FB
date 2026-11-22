
#include "cmixer.h"
#include <stdio.h>
#include <string.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"baseline failed line %d\n",__LINE__);return 1;}}while(0)
static int reads,depth,badlock;
static void locks(cm_Event*e){if(e->type==CM_EVENT_LOCK){if(depth++)badlock=1;}else if(e->type==CM_EVENT_UNLOCK){if(--depth)badlock=1;}}
static void tone(cm_Event *e){if(e->type==CM_EVENT_SAMPLES){reads++;for(int i=0;i<e->length;i+=2){e->buffer[i]=24000;e->buffer[i+1]=-12000;}}}
static void le16(unsigned char *b,unsigned x){b[0]=(unsigned char)x;b[1]=(unsigned char)(x>>8);}
static void le32(unsigned char*b,unsigned x){for(int i=0;i<4;i++)b[i]=(unsigned char)(x>>(8*i));}
int main(void){
 cm_init(48000);cm_set_lock(locks);
 cm_SourceInfo info={tone,0,48000,1024};
 cm_Source*a=cm_new_source(&info),*b=cm_new_source(&info);REQUIRE(a&&b);
 cm_play(a);cm_play(b);cm_Int16 out[640];cm_process(out,640);
 for(int i=0;i<320;i++){REQUIRE(out[2*i]==32767);REQUIRE(out[2*i+1]==-24000);}
 REQUIRE(reads>0);REQUIRE(!badlock&&depth==0);
 cm_destroy_source(b);cm_destroy_source(a);
 printf("MIX_BASELINE_OK frames=320 left=32767 right=-24000 reads=%d\n",reads);
 unsigned char wav[108]={0};memcpy(wav,"RIFF",4);le32(wav+4,100);memcpy(wav+8,"WAVEfmt ",8);le32(wav+16,16);
 le16(wav+20,1);le16(wav+22,2);le32(wav+24,48000);le32(wav+28,192000);le16(wav+32,4);le16(wav+34,16);
 memcpy(wav+36,"data",4);le32(wav+40,64);
 for(int i=0;i<16;i++){le16(wav+44+4*i,1000);le16(wav+46+4*i,(unsigned short)-2000);}
 cm_Source*w=cm_new_source_from_mem(wav,sizeof(wav));REQUIRE(w);cm_play(w);cm_process(out,64);
 for(int i=0;i<32;i++){REQUIRE(out[2*i]==(i<16?1000:0));REQUIRE(out[2*i+1]==(i<16?-2000:0));}
 REQUIRE(cm_get_state(w)==CM_STATE_STOPPED);cm_destroy_source(w);
 REQUIRE(!badlock&&depth==0);puts("WAV_BASELINE_OK frames=16 eof_silence=16 lock_balanced=1");return 0;
}
