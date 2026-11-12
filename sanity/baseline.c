#include "tinyspline.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
 tsBSpline a=ts_bspline_init(), b=ts_bspline_init();
 tsStatus status; tsDeBoorNet x=ts_deboornet_init(),y=ts_deboornet_init();
 tsReal cp[]={0,0, 1,2, 2,-1, 3,1, 4,0};
 assert(ts_bspline_new(5,2,3,TS_CLAMPED,&a,&status)==TS_SUCCESS);
 assert(ts_bspline_set_control_points(&a,cp,&status)==TS_SUCCESS);
 assert(ts_bspline_elevate_degree(&a,1,1e-12,&b,&status)==TS_SUCCESS);
 for(int i=0;i<=40;i++) {
  assert(ts_bspline_eval(&a,i/40.0,&x,&status)==TS_SUCCESS);
  assert(ts_bspline_eval(&b,i/40.0,&y,&status)==TS_SUCCESS);
  const tsReal *p=ts_deboornet_result_ptr(&x),*q=ts_deboornet_result_ptr(&y);
  assert(hypot(p[0]-q[0],p[1]-q[1])<1e-10);
 }
 printf("BASELINE elevation geometry PASS degree=%zu control_points=%zu\n",ts_bspline_degree(&b),ts_bspline_num_control_points(&b));
 ts_deboornet_free(&x);ts_deboornet_free(&y);ts_bspline_free(&a);ts_bspline_free(&b);
}
