#include "fixture.h"
#include "caption/mpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int caption_fixture_nal(uint8_t **nal,size_t *size){caption_frame_t f;sei_t s;uint8_t uuid[16]={'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'};
 caption_frame_init(&f);caption_frame_from_text(&f,"HI");f.timestamp=1.25;sei_init(&s,1.25);
 if(sei_from_caption_frame(&s,&f)==LIBCAPTION_ERROR){sei_free(&s);return 1;}
 sei_message_append(&s,sei_message_new(sei_type_user_data_unregistered,uuid,16));
 *nal=malloc(sei_render_size(&s));if(!*nal){sei_free(&s);return 1;}*size=sei_render(&s,*nal);sei_free(&s);return *size?0:1;}
static void be(FILE*f,unsigned v,unsigned n){while(n)fputc((v>>(8*--n))&255,f);}
static int tag(FILE*f,unsigned t,unsigned ts,const uint8_t*p,size_t n){fputc(t,f);be(f,(unsigned)n,3);be(f,ts&0xffffff,3);fputc(ts>>24,f);be(f,0,3);fwrite(p,1,n,f);be(f,11+(unsigned)n,4);return ferror(f)!=0;}
int caption_fixture_write(const char *path){FILE*f;uint8_t *nal=0,*body=0;size_t n;int r=1;
 static const uint8_t hdr[13]={'F','L','V',1,5,0,0,0,9,0,0,0,0};
 static const uint8_t script[]={2,0,4,'t','e','s','t'},audio[]={0xaf,1,0x10};
 /* AVC configuration has no SPS/PPS because this fixture carries SEI + AUD only. */
 static const uint8_t config[]={0x17,0,0,0,0,1,66,0,30,0xff,0xe0,0};
 if(caption_fixture_nal(&nal,&n))return 1;
 body=malloc(n+15);if(!body){free(nal);return 1;}body[0]=0x27;body[1]=1;body[2]=0;body[3]=0;body[4]=2;
 body[5]=n>>24;body[6]=n>>16;body[7]=n>>8;body[8]=n;memcpy(body+9,nal,n);
 body[9+n]=0;body[10+n]=0;body[11+n]=0;body[12+n]=2;body[13+n]=9;body[14+n]=0xf0;
 f=fopen(path,"wb");if(!f)goto done;fwrite(hdr,1,13,f);
 r=tag(f,18,0,script,sizeof(script))||tag(f,8,0x01000001,audio,sizeof(audio))||tag(f,9,0,config,sizeof(config))||tag(f,9,1250,body,n+15);
 if(fclose(f))r=1;
 done:free(nal);free(body);return r;}
