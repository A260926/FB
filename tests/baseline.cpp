#include "ForwardRasterizer.h"
#include "DeferredRasterizer.h"
#include "PERSPECTIVECAMERA.H"
#include "Material.h"
#include <cmath>
#include <cstdio>
struct ForwardProbe:ForwardRasterizer {using ForwardRasterizer::ForwardRasterizer;const std::vector<RGBColor>& pixels()const{return m_pixels;}};
struct DeferredProbe:DeferredRasterizer {using DeferredRasterizer::DeferredRasterizer;const std::vector<RGBColor>& pixels()const{return m_pixels;}};
template<class R> int check(){
 World world; R renderer(&world);
 PerspectiveCamera camera({0,0,0},{0,0,1},16,24,&renderer,1,30,90);
 world.m_camera=&camera;
 FlatMaterial material(RGBColor(.4f),RGBColor(0),1);
 std::vector<Point3D> verts{{-2,-2,5},{2,-2,5},{0,2,5}};
 std::vector<RGBColor> colors(3,RGBColor(1,0,0));
 GeometryObject object(&material,verts,colors,{}, {0,1,2});world.m_objects={&object};
 renderer.render(true,false);
 const size_t idx=8*24+12; auto depth=renderer.get_depth_buffer();auto color=renderer.pixels()[idx];
 if(std::abs(depth[idx]-5)>1e-5||std::abs(color.r-1.0)>1e-5||std::abs(color.g)>1e-6) return 1;
 auto p=camera.viewTransform({.5,.25,5});auto ndc=camera.projectTransform(p);auto back=camera.projectTransformInv(ndc,5);auto w=camera.viewTransformInv(back);
 if(std::abs(w.x-.5)>1e-5||std::abs(w.y-.25)>1e-5||std::abs(w.z-5)>1e-5)return 2;
 size_t covered=0;for(double d:depth)if(d<30)++covered;
 if(!covered)return 3;
 std::printf("BASELINE_PATH_OK covered=%zu center_depth=%.1f center_red=%.3f\n",covered,depth[idx],color.r);
 return 0;
}
int main(){if(check<ForwardProbe>())return 1;if(check<DeferredProbe>())return 2;std::puts("BASELINE_CAMERA_RENDER_OK");}
