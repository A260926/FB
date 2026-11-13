#include <cstdio>
#include <cstring>
#include "raster_api.h"
int main(){
 FT_Vector points[]={{64,64},{192,64},{192,192},{64,192}};
 char tags[]={1,1,1,1};short contours[]={3};FT_Outline outline{};
 outline.n_contours=1;outline.n_points=4;outline.points=points;outline.tags=tags;outline.contours=contours;
 unsigned char pixels[25]{};FT_Bitmap bitmap{};bitmap.width=5;bitmap.rows=5;bitmap.pitch=5;bitmap.buffer=pixels;bitmap.pixel_mode=FT_PIXEL_MODE_GRAY;bitmap.num_grays=256;
 FT_Raster raster;auto& api=ft_grays_raster;if(api.raster_new(nullptr,&raster))return 1;
 FT_Raster_Params params{};params.source=&outline;params.target=&bitmap;params.flags=FT_RASTER_FLAG_AA;
 if(api.raster_render(raster,&params))return 2;
 for(int row=0;row<5;++row)for(int x=0;x<5;++x){int y=4-row;int want=x>=1&&x<3&&y>=1&&y<3?255:0;if(pixels[row*5+x]!=want)return 3;}
 api.raster_done(raster);std::puts("BASELINE_OK rectangle_pixels=4 coverage=255");
}
