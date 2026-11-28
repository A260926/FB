#include "Scene.hpp"
#include "Triangle.hpp"
#include "RayTracer.hpp"
#include "Camera.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include <sstream>
#include <cassert>
#include <cmath>
#include <iostream>
using namespace hxa7241_minilight;
int main(){
 std::istringstream data("(0 0 0) (0 0 0)\n(-20 -20 2) (0 20 2) (20 -20 2) (0 0 0) (4 9 16)\n");data.exceptions(std::ios::badbit|std::ios::failbit);
 Scene scene(data,Vector3f(0,0,0));Random random;RayTracer tracer(scene);
 auto l=tracer.getRadiance(Vector3f(0,0,0),Vector3f(0,0,1),random);assert(std::abs(l[0]-4)<1e-10&&std::abs(l[1]-9)<1e-10&&std::abs(l[2]-16)<1e-10);
 const Triangle* hit=nullptr;Vector3f p;scene.getIntersection(Vector3f(0,0,0),Vector3f(0,0,1),nullptr,hit,p);assert(hit&&std::abs(p[2]-2)<1e-10);assert(scene.getEmittersCount()==1);
 auto black=tracer.getRadiance(Vector3f(50,0,0),Vector3f(0,0,1),random);assert(black.isZero());
 std::istringstream c("(0 0 0) (0 0 1) 10"),a("4 3"),b("4 3");Camera camera(c);Image im1(a),im2(b);Random x,y;camera.getFrame(scene,x,im1);camera.getFrame(scene,y,im2);std::ostringstream o1,o2;im1.getFormatted(o1,1);im2.getFormatted(o2,1);assert(o1.str()==o2.str()&&o1.str().find("P6")==0);assert(x.getInt32u()==y.getInt32u());
 std::cout<<"BASELINE primary=(4,9,16) indexed_hit_z=2 black_miss=1 camera=4x3 deterministic_ppm=1\n";
}
