#ifndef COLOR_FIXTURE_H
#define COLOR_FIXTURE_H
#include "src/webp/encode.h"
static int make_color_webp(WebPMemoryWriter *writer){
 WebPConfig c;WebPPicture p;if(!WebPConfigInit(&c)||!WebPPictureInit(&p))return 0;
 c.quality=85;c.method=3;c.exact=1;p.width=19;p.height=17;p.colorspace=WEBP_YUV420A;
 if(!WebPPictureAlloc(&p))return 0;
 for(int y=0;y<p.height;y++)for(int x=0;x<p.width;x++){
  p.y[y*p.y_stride+x]=(uint8_t)(16+(x*19+y*7)%220);
  p.a[y*p.a_stride+x]=(uint8_t)((x*31+y*13)%256);
 }
 for(int y=0;y<(p.height+1)/2;y++)for(int x=0;x<(p.width+1)/2;x++){
  p.u[y*p.uv_stride+x]=(uint8_t)(16+(x*43+y*11)%225);
  p.v[y*p.uv_stride+x]=(uint8_t)(16+(x*17+y*47)%225);
 }
 WebPMemoryWriterInit(writer);p.writer=WebPMemoryWrite;p.custom_ptr=writer;
 int ok=WebPEncode(&c,&p);WebPPictureFree(&p);return ok;
}
#endif
