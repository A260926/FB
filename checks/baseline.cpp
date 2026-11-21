#include "task_support/legacy.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void ck(bool b){if(!b){std::fprintf(stderr,"BASELINE_FAIL\n");std::exit(2);}}
static double energy(Vec v){ck(std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z));ck(v.x>=0&&v.y>=0&&v.z>=0);return v.x+v.y+v.z;}
int main(){
 unsigned short a[3]={1,2,3},b[3]={4,5,6},c[3]={7,8,9},e[3]={10,11,12};
 double d=energy(radiance(Ray(Vec(50,50,80),Vec(0,-1,0)),0,a));
 double m=energy(radiance(Ray(Vec(27,16.5,120),Vec(0,0,-1)),0,b));
 double g=energy(radiance(Ray(Vec(73,16.5,120),Vec(0,0,-1)),0,c));
 Vec light=radiance(Ray(Vec(50,80,81.6),Vec(0,1,0)),0,e);ck(std::abs(light.x-12)<1e-9&&std::abs(light.y-12)<1e-9&&std::abs(light.z-12)<1e-9);
 unsigned short f[3]={13,14,15};Vec miss=radiance(Ray(Vec(50,50,500000),Vec(0,0,1)),0,f);ck(energy(miss)==0);
 std::printf("BASELINE diffuse=%.9f mirror=%.9f glass=%.9f emitter=%.9f\n",d,m,g,energy(light));
 std::puts("LEGACY_RECURSION_OK miss_black=1 emitter_rgb=12,12,12");
}
