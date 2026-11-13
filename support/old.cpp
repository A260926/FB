#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"
#include <cstdio>
#define WIDTH 640
#define HEIGHT 640
#include "testing/pgl_test_expect.h"
static unsigned old_checks=0;
#undef PGL_EXPECT
#define PGL_EXPECT(cond,msg) do { ++old_checks; if(!(cond)){std::fprintf(stderr,"FAIL %s: %s\n",__func__,msg);++pgl_test_fails;} } while(0)
glContext the_Context;
#include "testing/guard_band_draw.c"
#include "testing/fill_watertight.c"
int main(){
 void (*tests[])(int,char**,void*)={guard_band_line_inset,guard_band_shared_edge,guard_band_far_vert,guard_band_w_negative,fill_watertight};
 for(auto test:tests){pix_t *pixels=nullptr;if(!init_glContext(&the_Context,&pixels,WIDTH,HEIGHT))return 2;test(0,nullptr,nullptr);free_glContext(&the_Context);}
 if(pgl_test_fails || old_checks!=13)return 3;
 std::printf("UPSTREAM_RASTER_OK cases=5 checks=%u\n",old_checks);
}
