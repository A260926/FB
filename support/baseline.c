#include "tinyspline.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static void check(int x){if(!x){fputs("baseline assertion failed\n",stderr);exit(1);}}
int main(void){
 tsBSpline a=ts_bspline_init(),b=ts_bspline_init(),d=ts_bspline_init();tsStatus s;size_t k;int i,j;tsReal maxerr=0;
 tsReal cp[]={0,0,0, 1,2,0, 2,-1,1, 3,0,1};
 check(ts_bspline_new(4,3,3,TS_CLAMPED,&a,&s)==TS_SUCCESS);
 check(ts_bspline_set_control_points(&a,cp,&s)==TS_SUCCESS);
 check(ts_bspline_insert_knot(&a,.5,2,&b,&k,&s)==TS_SUCCESS);
 check(ts_bspline_num_control_points(&b)==6);
 for(i=0;i<=100;++i){tsDeBoorNet x=ts_deboornet_init(),y=ts_deboornet_init();tsReal *px=0,*py=0;
  check(ts_bspline_eval(&a,i/100.,&x,&s)==TS_SUCCESS);check(ts_bspline_eval(&b,i/100.,&y,&s)==TS_SUCCESS);
  check(ts_deboornet_result(&x,&px,&s)==TS_SUCCESS);check(ts_deboornet_result(&y,&py,&s)==TS_SUCCESS);
  for(j=0;j<3;++j){tsReal e=fabs(px[j]-py[j]);if(e>maxerr)maxerr=e;}free(px);free(py);ts_deboornet_free(&x);ts_deboornet_free(&y);
 }
 check(maxerr<1e-12);check(ts_bspline_derive(&b,1,1e-9,&d,&s)==TS_SUCCESS);check(ts_bspline_degree(&d)==2);
 printf("TINYSPLINE_BASELINE controls=4->6 samples=101 shape_error=%.3g derivative_degree=2\n",(double)maxerr);
 puts("OBSERVED: insertion preserves shape; no knot removal or simplification API in source");
 ts_bspline_free(&a);ts_bspline_free(&b);ts_bspline_free(&d);return 0;
}
