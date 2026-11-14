#include <cassert>
#include <cmath>
#include <cstdio>
#include "noise/noise.h"
void graph_baseline() {
  using namespace noise::module;
  Perlin control; control.SetSeed(17); control.SetOctaveCount(3);
  RidgedMulti ridges; ridges.SetSeed(11); ridges.SetOctaveCount(4);
  Const flat; flat.SetConstValue(0.15);
  Select select; select.SetSourceModule(0,flat); select.SetSourceModule(1,ridges);
  select.SetControlModule(control); select.SetBounds(-0.3,0.4); select.SetEdgeFalloff(0.12);
  Turbulence warp; warp.SetSourceModule(0,select); warp.SetSeed(31); warp.SetPower(0.17); warp.SetRoughness(3);
  RotatePoint rotate; rotate.SetSourceModule(0,warp); rotate.SetAngles(19,31,-13);
  Cache cache; cache.SetSourceModule(0,rotate);
  double sum=0;
  for(int i=0;i<30;i++) {
    double x=0.13+i*0.071,y=-0.27+i*0.033,z=0.41-i*0.017;
    double v=cache.GetValue(x,y,z); assert(std::isfinite(v));
    assert(v==cache.GetValue(x,y,z)); assert(v==rotate.GetValue(x,y,z)); sum+=v;
  }
  assert(std::abs(sum)>0.01); std::printf("BASELINE warped-ridged-graph PASS samples=30 sum=%.12g\n",sum);
}
#include "noiseutils.h"
int main(){
 graph_baseline();
 using namespace noise; using namespace noise::utils;
 module::Perlin perlin; perlin.SetSeed(31);perlin.SetOctaveCount(3);
 NoiseMap h;NoiseMapBuilderPlane b;b.SetSourceModule(perlin);b.SetDestNoiseMap(h);b.SetDestSize(7,5);b.SetBounds(-.7,1.4,.2,1.1);b.EnableSeamless();b.Build();
 double sum=0;
 for(int j=0;j<5;j++)for(int i=0;i<7;i++){
  double x=-.7+i*2.1/7,z=.2+j*.9/5,wx=1.-i/7.,wz=1.-j/5.;
  double a=perlin.GetValue(x,0,z),c=perlin.GetValue(x+2.1,0,z),d=perlin.GetValue(x,0,z+.9),e=perlin.GetValue(x+2.1,0,z+.9);
  double v=(a+(c-a)*wx)*(1-wz)+(d+(e-d)*wx)*wz;
  assert(std::fabs(h.GetValue(i,j)-v)<2e-7);sum+=h.GetValue(i,j);
 }
 Image out;out.SetSize(7,5);RendererNormalMap r;r.SetSourceNoiseMap(h);r.SetDestImage(out);r.SetBumpHeight(2.);r.EnableWrap();r.Render();
 for(int j=0;j<5;j++)for(int i=0;i<7;i++){
  double dx=2.*(h.GetValue(i,j)-h.GetValue((i+1)%7,j));
  double dy=2.*(h.GetValue(i,j)-h.GetValue(i,(j+1)%5));
  double den=std::sqrt(dx*dx+dy*dy+1.);Color c=out.GetValue(i,j);
  assert(c.red==(unsigned)std::floor((dx/den+1.)*127.5));
  assert(c.green==(unsigned)std::floor((dy/den+1.)*127.5));
  assert(c.blue==(unsigned)std::floor((1./den+1.)*127.5));
 }
 std::printf("BASELINE procedural-texture PASS pixels=35 sum=%.12g\n",sum);
}
