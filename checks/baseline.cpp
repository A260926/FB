#include "tinysplinecxx.h"
#include <cmath>
#include <iostream>
int main(){
 tinyspline::BSpline curve(4,2,3);
 curve.setControlPoints({-1,0,2,3,-2,3,1,0});
 auto p=curve.eval(.5).result();
 auto bezier=curve.toBeziers();
 auto q=bezier.eval(.5).result();
 if(std::abs(p[0])>1e-12||std::abs(p[1]-2.25)>1e-12)return 1;
 if(std::abs(p[0]-q[0])+std::abs(p[1]-q[1])>1e-12)return 2;
 auto copy=curve;curve.setControlPoints({0,0,0,0,0,0,0,0});
 auto r=copy.eval(.5).result();if(std::abs(r[1]-2.25)>1e-12)return 3;
 std::cout<<"TINYSPLINE_BASELINE_OK midpoint=0,2.25 snapshot_copy=1\n";
}
