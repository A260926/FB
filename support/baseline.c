#include <stdio.h>
#include <stdint.h>
#include "src/webp/decode.h"
#include "fixture.h"
int verify_alpha_order(const WebPMemoryWriter *writer);
int main(void){
 WebPMemoryWriter w;WebPDecoderConfig c;if(!make_color_webp(&w)||!WebPInitDecoderConfig(&c))return 2;
 c.output.colorspace=MODE_RGBA;
 if(WebPDecode(w.mem,w.size,&c)!=VP8_STATUS_OK)return 3;
 if(c.output.width!=19||c.output.height!=17)return 4;
 uint32_t h=2166136261u;
 for(int y=0;y<17;y++)for(int x=0;x<19;x++){
  const uint8_t *p=c.output.u.RGBA.rgba+y*c.output.u.RGBA.stride+x*4;
  if(p[3]!=(uint8_t)((x*31+y*13)%256))return 5;
  for(int z=0;z<4;z++)h=(h^p[z])*16777619u;
 }
 if(h!=0x81fe2f26u)return 6;
 if(verify_alpha_order(&w))return 7;
 printf("WEBP_COLOR_BASE pixels=323 alpha=1 checksum=%08x\n",h);
 WebPFreeDecBuffer(&c.output);WebPMemoryWriterClear(&w);return 0;
}
