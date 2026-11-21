#include <tinynurbs/tinynurbs.h>
#include <cmath>
#include <cstdio>
int main(){
 tinynurbs::RationalSurface3d s; s.degree_u=2;s.degree_v=1;
 s.knots_u={2,2,2,5,5,5};s.knots_v={-1,-1,3,3};
 s.control_points={3,2,{glm::dvec3(1,0,0),glm::dvec3(1,0,2),glm::dvec3(1,1,0),glm::dvec3(1,1,2),glm::dvec3(0,1,0),glm::dvec3(0,1,2)}};
 double w=std::sqrt(.5);s.weights={3,2,{1,1,w,w,1,1}};
 if(!tinynurbs::surfaceIsValid(s))return 1;
 auto p=tinynurbs::surfacePoint(s,3.5,1.0); auto d=tinynurbs::surfaceDerivatives(s,1,3.5,1.0);
 auto halves=tinynurbs::surfaceSplitU(s,3.5);
 double gap=glm::length(p-tinynurbs::surfacePoint(std::get<0>(halves),3.5,1.0));
 double n=glm::length(glm::cross(d(1,0),d(0,1)));
 if(std::abs(p.x-w)>1e-12||std::abs(p.y-w)>1e-12||std::abs(p.z-1)>1e-12||gap>1e-12||n<.1)return 2;
 std::printf("RATIONAL_PRODUCTION_OK point=%.12f,%.12f,%.12f split_gap=%.3g normal_length=%.12f\n",p.x,p.y,p.z,gap,n);
 return 0;
}
