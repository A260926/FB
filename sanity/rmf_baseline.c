#include "tinyspline.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    tsBSpline s=ts_bspline_init(); tsStatus st; tsFrame a[5], b[6];
    tsReal good[]={0,.2,.4,.7,1}, repeated[]={0,.2,.2,.4,.7,1};
    size_t i,j; int bad=0;
    assert(ts_bspline_new_with_control_points(5,3,3,TS_CLAMPED,&s,&st,
        100.,200.,0., 150.,220.,10., 190.,120.,50.,
        260.,70.,30., 300.,200.,20.)==TS_SUCCESS);
    assert(ts_bspline_compute_rmf(&s,good,5,0,a,&st)==TS_SUCCESS);
    for(i=0;i<5;i++) for(j=0;j<3;j++) assert(isfinite(a[i].normal[j]));
    puts("RMF regular samples PASS");
    assert(ts_bspline_compute_rmf(&s,repeated,6,0,b,&st)==TS_SUCCESS);
    for(i=0;i<6;i++) for(j=0;j<3;j++) bad+=!isfinite(b[i].normal[j]);
    assert(bad>0); printf("RMF repeated sample nonfinite components %d\n",bad);
    ts_bspline_free(&s); return 0;
}
