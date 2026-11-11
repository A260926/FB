#define CANVAS_ITY_IMPLEMENTATION
#include "canvas_ity.hpp"
#include <cassert>
#include <cstdio>
using namespace canvas_ity;
int main(){canvas c(6,2);c.set_color(fill_style,1,0,0,1);c.set_global_alpha(.5f);c.fill_rectangle(0,0,4,2);c.fill_rectangle(2,0,4,2);unsigned char b[48];c.get_image_data(b,6,2,24,0,0);assert(b[3]==127||b[3]==128);assert(b[2*4+3]==191||b[2*4+3]==192);printf("PASS per-primitive alpha left=%u overlap=%u\n",b[3],b[11]);
canvas k(3,1);k.save();k.begin_path();k.rectangle(0,0,1,1);k.clip();k.set_color(fill_style,0,1,0,1);k.fill_rectangle(0,0,3,1);k.restore();k.set_color(fill_style,0,0,1,1);k.fill_rectangle(2,0,1,1);unsigned char d[12];k.get_image_data(d,3,1,12,0,0);assert(d[3]==255&&d[7]==0&&d[11]==255);puts("PASS saved clip restored");}
