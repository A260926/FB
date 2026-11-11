#include "plutovg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int lines;
static void visit(void *u, plutovg_path_command_t c,const plutovg_point_t *p,int n){(void)u;(void)n;if(c==PLUTOVG_PATH_COMMAND_LINE_TO){lines++;printf("line %.1f %.1f\n",p[0].x,p[0].y);}}
int main(void){
 plutovg_path_t *p=plutovg_path_create();
 plutovg_path_move_to(p,1,2);plutovg_path_line_to(p,13,2);
 float d[]={3,2,1};
 plutovg_path_traverse_dashed(p,-1,d,3,visit,NULL);assert(lines==3);
 plutovg_path_t *q=plutovg_path_create();
 /* Independent expected intervals for odd [3,2,1], offset -1. */
 plutovg_path_move_to(q,2,2);plutovg_path_line_to(q,5,2);
 plutovg_path_move_to(q,7,2);plutovg_path_line_to(q,8,2);
 plutovg_path_move_to(q,11,2);plutovg_path_line_to(q,13,2);
 plutovg_surface_t *a=plutovg_surface_create(16,5),*b=plutovg_surface_create(16,5);
 plutovg_canvas_t *ca=plutovg_canvas_create(a),*cb=plutovg_canvas_create(b);
 plutovg_canvas_set_rgba(ca,1,0,0,1);plutovg_canvas_set_rgba(cb,1,0,0,1);
 plutovg_canvas_set_line_width(ca,1);plutovg_canvas_set_line_width(cb,1);
 plutovg_canvas_set_dash(ca,-1,d,3);plutovg_canvas_stroke_path(ca,p);plutovg_canvas_stroke_path(cb,q);
 assert(!memcmp(plutovg_surface_get_data(a),plutovg_surface_get_data(b),plutovg_surface_get_stride(a)*5));
 int nonzero=0;unsigned char *data=plutovg_surface_get_data(a);for(int i=0;i<plutovg_surface_get_stride(a)*5;i++)nonzero+=data[i]!=0;assert(nonzero>0);
 printf("PASS independent interval/stroke nonzero=%d\n",nonzero);
 plutovg_canvas_destroy(ca);plutovg_canvas_destroy(cb);plutovg_surface_destroy(a);plutovg_surface_destroy(b);plutovg_path_destroy(p);plutovg_path_destroy(q);return 0;
}
