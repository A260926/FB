#include "tinyspline.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static int fail(tsError e, tsStatus *s) {
    if (e) { fprintf(stderr, "upstream error: %s\n", s->message); return 1; }
    return 0;
}
int main(void) {
    tsBSpline a = ts_bspline_init(), b = ts_bspline_init(), d = ts_bspline_init();
    tsDeBoorNet n = ts_deboornet_init(), m = ts_deboornet_init(), q = ts_deboornet_init();
    tsStatus status;
    tsReal cp[] = {0,0,0, 1,2,1, 2,-1,0, 3,2,-1, 4,0,0};
    size_t i, j; double maxerr = 0;
    if (fail(ts_bspline_new(5,3,3,TS_CLAMPED,&a,&status),&status) ||
        fail(ts_bspline_set_control_points(&a,cp,&status),&status) ||
        fail(ts_bspline_to_beziers(&a,&b,&status),&status) ||
        fail(ts_bspline_derive(&a,1,0,&d,&status),&status)) return 1;
    for (i=0; i<4; ++i) {
        tsReal u = (2*i+1)/8.0;
        const tsReal *p, *r;
        if (fail(ts_bspline_eval(&a,u,&n,&status),&status) ||
            fail(ts_bspline_eval(&b,u,&m,&status),&status)) return 1;
        p=ts_deboornet_result_ptr(&n); r=ts_deboornet_result_ptr(&m);
        for(j=0;j<3;++j) { double e=fabs(p[j]-r[j]); if(e>maxerr)maxerr=e; }
        ts_deboornet_free(&n); ts_deboornet_free(&m);
    }
    if (fail(ts_bspline_bisect(&a,2,1e-12,1,0,1,60,&q,&status),&status)) return 1;
    if (maxerr > 1e-12 || fabs(ts_deboornet_knot(&q)-.5)>1e-12 ||
        ts_bspline_num_control_points(&b)!=8 || ts_bspline_degree(&d)!=2) return 2;
    printf("BASELINE_TINYSPLINE_OK dim=3 pieces=2 samples=4 bisect=0.5\n");
    printf("decomposition_max_error=%.17g derivative_degree=%lu\n", maxerr, (unsigned long)ts_bspline_degree(&d));
    ts_bspline_free(&a); ts_bspline_free(&b); ts_bspline_free(&d); ts_deboornet_free(&q);
    return 0;
}
