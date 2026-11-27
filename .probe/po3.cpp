#include <cstdio>
#include <vector>
#include "raster_api.h"
#include "reference.h"
#include "fixtures.h"
int main(){
  fx::Shape s=fx::bandsPressure();
  FT_Outline o=s.outline();
  int W=110,H=175,padL=3,padR=5,padRows=2,stride=padL+W+padR,nrows=H+2*padRows;
  std::vector<unsigned char> buf((size_t)stride*nrows,0xA5);
  FT_Bitmap bm{};bm.width=W;bm.rows=H;bm.num_grays=256;bm.pixel_mode=FT_PIXEL_MODE_GRAY;
  size_t rowLo=padRows,rowHi=padRows+H;
  bm.buffer=buf.data()+rowLo*stride+padL; bm.pitch=stride;
  FT_Raster r;ft_grays_raster.raster_new(nullptr,&r);
  FT_Raster_Params p{};p.source=&o;p.target=&bm;p.flags=FT_RASTER_FLAG_AA;
  int e=ft_grays_raster.raster_render(r,&p);ft_grays_raster.raster_done(r);
  printf("err=%d\n",e);
  auto rr=ref::render(o,W,H,0,5e-5);
  // find first nonzero physical pixel
  for(size_t pr=0;pr<(size_t)nrows;++pr){int nz=0;for(int x=0;x<stride;++x)if(buf[pr*stride+x]!=0xA5&&buf[pr*stride+x]!=0)nz++;printf("phys %zu: nz=%d first=%d\n",pr,nz,(int)buf[pr*stride+padL]);}
  printf("mapping y=0 -> phys %zu: ",(size_t)(rowHi-1));for(int x=0;x<8;++x)printf("%d ",(int)buf[(rowHi-1)*stride+padL+x]);printf("\n");
  printf("ref y=0: ");for(int x=0;x<8;++x)printf("%.0f ",rr.gray[(size_t)x]);printf("\n");
  printf("mapping y=174 -> phys %zu: ",(size_t)rowLo);for(int x=0;x<8;++x)printf("%d ",(int)buf[rowLo*stride+padL+x]);printf("\n");
  printf("ref y=174: ");for(int x=0;x<8;++x)printf("%.0f ",rr.gray[(size_t)174*W+x]);printf("\n");
  return 0;
}
