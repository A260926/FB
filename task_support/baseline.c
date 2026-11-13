#include "plutovg.h"
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
int main(void) {
 plutovg_surface_t* s=plutovg_surface_create(24,16);
 plutovg_canvas_t* c=plutovg_canvas_create(s);
 plutovg_canvas_set_rgba(c,1,0,0,1);plutovg_canvas_fill_rect(c,2,2,8,8);
 uint32_t* row=(uint32_t*)(plutovg_surface_get_data(s)+4*plutovg_surface_get_stride(s));
 assert(row[4]==0xffff0000u && row[0]==0);
 plutovg_gradient_stop_t stops[2]={{0,{0,0,0,1}},{1,{1,1,1,1}}};
 plutovg_canvas_save(c);
 plutovg_canvas_set_linear_gradient(c,12,0,24,0,PLUTOVG_SPREAD_METHOD_PAD,stops,2,NULL);
 plutovg_canvas_fill_rect(c,12,0,12,16);
 assert((row[13]&255)<(row[22]&255));
 plutovg_canvas_restore(c);plutovg_canvas_fill_rect(c,0,12,4,4);
 row=(uint32_t*)(plutovg_surface_get_data(s)+13*plutovg_surface_get_stride(s));assert(row[1]==0xffff0000u);
 puts("PLUTOVG_SOLID_GRADIENT_STATE_OK");
 plutovg_canvas_destroy(c);plutovg_surface_destroy(s);return 0;
}
