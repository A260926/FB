#include <cstdio>
#include <vector>
#include "raster_api.h"
#include "fixtures.h"
int main(){
  fx::Shape s=fx::rect(fx::px(1),fx::px(1),fx::px(3),fx::px(3));
  FT_Outline o=s.outline();
  int W=5,H=5,padL=2,padR=2,padRows=1,stride=padL+W+padR,nrows=H+2*padRows;
  for(int ps : {1,-1}){
    std::vector<unsigned char> buf((size_t)stride*nrows,0xA5);
    FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;bm.pitch=ps*stride;
    size_t rowLo=padRows,rowHi=padRows+H;
    unsigned char* top=buf.data()+rowLo*stride+padL;
    unsigned char* bot=buf.data()+(rowHi-1)*stride+padL;
    bm.buffer = ps>0? top : bot;
    FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);
    FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
    ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
    printf("pitch=%d buffer=%s\n",ps,ps>0?"top":"bottom");
    for(int pr=0;pr<nrows;++pr){for(int x=0;x<stride;++x)printf("%3d ",buf[(size_t)pr*stride+x]);printf("\n");}
    // reconstruct bottom-up
    unsigned char* origin = ps>0? bot : bot;
    printf("reconstruct (y=0 bottom):\n");
    for(int y=0;y<H;++y){const unsigned char* rp=origin-(long)ps*(long)stride*y;for(int x=0;x<W;++x)printf("%3d ",rp[x]);printf("\n");}
  }
  return 0;
}
