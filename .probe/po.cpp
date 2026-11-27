#include <cstdio>
#include <vector>
#include "raster_api.h"
#include "fixtures.h"
static void go(int pitchSign){
  fx::Shape s=fx::rect(fx::px(1),fx::px(1),fx::px(3),fx::px(3));
  FT_Outline o=s.outline();
  int W=5,H=5,stride=W,nrows=H;
  std::vector<unsigned char> buf((size_t)stride*nrows,0xA5);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  bm.pitch=pitchSign*stride;
  bm.buffer = pitchSign>0 ? buf.data() : buf.data()+(size_t)(nrows-1)*stride;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);
  FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  printf("pitch=%d buffer=%s\n",pitchSign,pitchSign>0?"top":"bottom");
  for(int pr=0;pr<nrows;++pr){for(int x=0;x<W;++x)printf("%3d ",buf[(size_t)pr*stride+x]);printf("\n");}
}
int main(){go(1);go(-1);return 0;}
