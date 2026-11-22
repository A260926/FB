#include "caption/caption.h"
#include "caption/mpeg.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"baseline line %d\n",__LINE__);return 1;}}while(0)
static int send(caption_frame_t*f,unsigned w,int n){return caption_frame_decode(f,eia608_parity(w),(double)n/30)!=LIBCAPTION_ERROR;}
int main(void){
 caption_frame_t f;caption_frame_init(&f);int n=0;
 CHECK(send(&f,0x1420,n++));CHECK(send(&f,0x1460,n++));CHECK(send(&f,0x4142,n++));CHECK(send(&f,0x142f,n++));
 CHECK(!strcmp(caption_frame_read_char(&f,14,0,0,0),"A"));CHECK(!strcmp(caption_frame_read_char(&f,14,1,0,0),"B"));
 CHECK(send(&f,0x142f,n++));CHECK(!strcmp(caption_frame_read_char(&f,14,0,0,0),"A"));
 CHECK(send(&f,0x1420,n++));CHECK(send(&f,0x1460,n++));CHECK(send(&f,0x4344,n++));
 CHECK(send(&f,0x1460,n++));CHECK(send(&f,0x1424,n++));
 CHECK(!strcmp(f.back.cell[14][0].data,"C"));
 CHECK(send(&f,0x142f,n++));CHECK(!strcmp(caption_frame_read_char(&f,14,0,0,0),"C"));
 CHECK(send(&f,0x1420,n++));CHECK(send(&f,0x142f,n++));CHECK(!*caption_frame_read_char(&f,14,0,0,0));
 caption_frame_t screen,out;caption_frame_init(&screen);screen.write=&screen.front;
 CHECK(caption_frame_write_char(&screen,14,0,eia608_style_white,0,"O"));
 CHECK(caption_frame_write_char(&screen,14,1,eia608_style_white,0,"K"));
 sei_t sei;sei_init(&sei,1);CHECK(sei_from_caption_frame(&sei,&screen)!=LIBCAPTION_ERROR);
 caption_frame_init(&out);CHECK(sei_to_caption_frame(&sei,&out)!=LIBCAPTION_ERROR);sei_free(&sei);
 CHECK(!strcmp(caption_frame_read_char(&out,14,0,0,0),"O"));CHECK(!strcmp(caption_frame_read_char(&out,14,1,0,0),"K"));
 puts("CAPTION_FIXED_SOURCE_OK EOC_duplicate=stable hidden_after_EOC=empty DER_tail=CD popon=OK");return 0;
}
