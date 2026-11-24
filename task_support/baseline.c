#include "caption/mpeg.h"
#include "fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NEED(x) do{if(!(x)){fprintf(stderr,"baseline check line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv){uint8_t *nal=0;size_t n;unsigned count=0;sei_t parsed;sei_message_t *m;caption_frame_t f;char text[4096];
 if(argc!=2)return 2;NEED(caption_fixture_nal(&nal,&n)==0);NEED(n>2&&nal[0]==6);
 NEED(sei_parse(&parsed,nal+1,n-1,1.25)!=LIBCAPTION_ERROR);
 for(m=sei_message_head(&parsed);m;m=sei_message_next(m))count++;
 NEED(count>=2);caption_frame_init(&f);NEED(sei_to_caption_frame(&parsed,&f)!=LIBCAPTION_ERROR);
 caption_frame_to_text(&f,text);NEED(strstr(text,"HI")!=0);NEED(f.timestamp==1.25);
 printf("BASELINE_CAPTION_PASS text=HI messages=%u timestamp=1.25\n",count);
 sei_free(&parsed);free(nal);NEED(caption_fixture_write(argv[1])==0);return 0;}
