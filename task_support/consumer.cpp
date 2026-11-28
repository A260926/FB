#include "Coherent.hpp"
#include "Scene.hpp"
#include "Triangle.hpp"
#include "Camera.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include <sstream>
#include <cmath>
using namespace hxa7241_minilight;
int main(){std::istringstream s("(0 0 0) (0 0 0)\n(-100 -100 2) (0 100 2) (100 -100 2) (0 0 0) (0 0 0)\n");s.exceptions(std::ios::badbit|std::ios::failbit);Scene scene(s,Vector3f(0,0,0));if(scene.getTriangleCount()!=1||scene.getTriangle(0).getArea()<=0)return 41;
 CoherentConfig c;c.lines.push_back({1,Vector3f(1,.5,.25)});OpticalSurface source;source.kind=OpticalKind::Source;source.intensity={4};source.coherence_group=7;c.surfaces.push_back(source);CoherentTracer tracer(scene,c);auto r=tracer.trace(Vector3f(0,0,0),Vector3f(0,0,1));if(r.queries!=1||r.terminal_paths!=1||r.truncated_paths||std::abs(r.radiance[0]-4)>1e-8||std::abs(std::norm(r.fields.at(7)[0])-4)>1e-8)return 42;
 std::istringstream cs("(0 0 0) (0 0 1) 10"),is("1 1");Camera camera(cs);Image im(is);Random random;camera.getCoherentFrame(tracer,random,im);auto p=im.getLinearPixel(0,0,1);if(std::abs(p[0]-4)>1e-8||std::abs(p[1]-2)>1e-8||std::abs(p[2]-1)>1e-8)return 43;return 0;}
