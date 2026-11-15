#include "yocto_scene.h"
#include "yocto_bvh.h"
#include "yocto_trace.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace yocto;
static void require(bool b){if(!b){std::fputs("baseline assertion failed\n",stderr);std::exit(1);}}
int main(){
 scene_data s; s.cameras.push_back({});s.cameras[0].aspect=1;
 material_data m; m.emission={2,1,0.5f};s.materials.push_back(m);
 shape_data shape;shape.positions={{-1,-1,0},{1,-1,0},{0,1,0}};shape.triangles={{0,1,2}};s.shapes.push_back(shape);
 instance_data a;a.shape=0;a.material=0;a.frame.o={0,0,-2};s.instances.push_back(a);
 auto b=a;b.frame.o={3,0,-2};s.instances.push_back(b);
 trace_params p;p.resolution=12;p.samples=1;p.noparallel=true;p.bounces=2;p.seed=42;
 auto bvh=make_trace_bvh(s,p);auto lights=make_trace_lights(s,p);
 require(lights.lights.size()==2);
 auto h=intersect_scene_bvh(bvh.bvh,s,{{0,0,0},{0,0,-1}});require(h.hit&&h.instance==0&&std::abs(h.distance-2)<1e-5f);
 auto h2=intersect_scene_bvh(bvh.bvh,s,{{3,0,0},{0,0,-1}});require(h2.hit&&h2.instance==1);
 auto state=make_trace_state(s,p);trace_samples(state,s,bvh,lights,p);double energy=0;
 for(auto c:state.image){require(std::isfinite(c.x)&&std::isfinite(c.y)&&std::isfinite(c.z));energy+=c.x+c.y+c.z;}
 require(state.samples==1&&energy>0);
 std::printf("CPU_TRACE samples=%d energy=%.6f shared_instances=2\n",state.samples,energy);
 s.shapes[0].positions[2].y=1.5f;s.instances[1].frame.o.x=4;
 update_scene_bvh(bvh.bvh,s,{1},{0});
 auto moved=intersect_scene_bvh(bvh.bvh,s,{{4,0,0},{0,0,-1}});require(moved.hit&&moved.instance==1);
 require(!intersect_scene_bvh(bvh.bvh,s,{{3,0.9f,0},{0,0,-1}}).hit);
 std::puts("CPU_REFIT shared_geometry=ok stable_instance=1");
}
