#define CANVAS_ITY_IMPLEMENTATION
#include "canvas_ity.hpp"
#include <cassert>
#include <cstdio>
int main(){
 canvas_ity::canvas c(16,12);c.set_color(canvas_ity::fill_style,1,0,0,1);c.set_shadow_color(0,0,1,0.5f);c.shadow_offset_x=6;
 c.fill_rectangle(2,2,3,3);unsigned char p[16*12*4];c.get_image_data(p,16,12,16*4,0,0);
 assert(p[(3*16+3)*4]==255&&p[(3*16+3)*4+3]==255);
 assert(p[(3*16+9)*4+2]==255&&p[(3*16+9)*4+3]>=127&&p[(3*16+9)*4+3]<=128);
 assert(p[3]==0);puts("CANVAS_OFFSET_SHADOW_BASELINE_OK");
}
