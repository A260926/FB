#include "Camera.hpp"
#include "Scene.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include "RayTracer.hpp"
#include "SurfacePoint.hpp"
#include <sstream>
#include <fstream>
#include <cmath>
#include <cstdio>
using namespace hxa7241_minilight;
using namespace hxa7241_general;
int main(){
 std::istringstream ci("(0 0 0) (0 0 1) 40"); Camera camera(ci);
 std::istringstream si("(0 0 0) (0 0 0)\n(-10 -10 1) (0 10 1) (10 -10 1) (0 0 0) (2 1 .5)\n");
 si.exceptions(std::ios::badbit|std::ios::failbit|std::ios::eofbit);
 Scene scene(si,camera.getEyePoint()); Random random; RayTracer tracer(scene);
 const Triangle* t=0;Vector3f pos;scene.getIntersection(Vector3f(0,0,0),Vector3f(0,0,1),0,t,pos);
 if(!t||std::fabs(pos[2]-1)>1e-10)return 1;
 Vector3f c=tracer.getRadiance(Vector3f(0,0,0),Vector3f(0,0,1),random);
 if(std::fabs(c[0]-2)>1e-12||std::fabs(c[1]-1)>1e-12||std::fabs(c[2]-.5)>1e-12)return 2;
 std::istringstream ii("4 4"); Image image(ii);camera.getFrame(scene,random,image);
 std::ostringstream out;image.getFormatted(out,1);std::string data=out.str();
 if(data.substr(0,2)!="P6"||data.size()<48)return 3;
 std::ofstream f(".task-build/baseline.ppm",std::ios::binary);f.write(data.data(),data.size());if(!f)return 4;
 std::printf("BASELINE native_spatial_hit z=%.6f radiance=%.6f,%.6f,%.6f camera_pixels=16 ppm_bytes=%zu PASS\n",pos[2],c[0],c[1],c[2],data.size());
}
