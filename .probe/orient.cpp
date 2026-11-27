#include <cstdio>
#include <vector>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
int main(){
  fx::Shape s=fx::bandsPressure().translated(0,0);
  FT_Outline o=s.outline();
  int W=110,H=175;
  std::vector<unsigned char> g((size_t)W*H,0xA5);
  int padL=3,padR=5,padR_rows=2;
  int stride=padL+W+padR, nrows=H+2*padR_rows;
  std::vector<unsigned char> buf((size_t)stride*nrows,0xA5);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.pitch=stride;bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  unsigned char* base=buf.data()+(size_t)(nrows-1-padR_rows)*stride+padL;
  bm.buffer=base;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);
  FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  // print bitmap row 0 (bottom) from validBase
  for(int x=0;x<30;++x){
    unsigned char* row=base- (long)stride*0;
    printf("%d ",(int)row[x]);
  }printf("\n");
  auto rr=ref::render(o,W,H,0,5e-5);
  for(int x=0;x<30;++x)printf("%.0f ",rr.gray[(size_t)0*W+x]);printf("\n");
  // also check raw buf top/bottom orientation
  return 0;
}
