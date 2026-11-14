#include "tinysplinecxx.h"
#include <cstdio>
#include <cmath>
#include <vector>
int main(){
 std::vector<tinyspline::real> p={0,0, 2,0, 2,1, -1,2, 0,0};
 auto s=tinyspline::BSpline::interpolateCubicNatural(p,2);
 auto a=s.eval(0).result(), b=s.eval(1).result();
 auto d=s.derive();auto da=d.eval(0).result(), db=d.eval(1).result();
 double gap=std::hypot(a[0]-b[0],a[1]-b[1]);double seam=std::hypot(da[0]-db[0],da[1]-db[1]);
 printf("NATURAL_CLOSED_POSITION gap=%.6f control_points=%zu\n",gap,s.numControlPoints());
 printf("NATURAL_SEAM first_derivative_gap=%.6f\n",seam);
 if(!std::isfinite(seam)||gap>1e-8||seam<1.0)return 1;
 puts("BASELINE_OK natural_is_not_periodic");return 0;
}
