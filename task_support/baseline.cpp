#include "ForwardRasterizer.h"
#include "OrthographicCamera.h"
#include "DIRECTIONALLIGHT.H"
#include "Material.h"
#include <cstdio>
#include <cmath>
class Probe: public ForwardRasterizer {public: Probe(const World* w):ForwardRasterizer(w){} size_t maps()const{return m_shadow_maps.size();}};
int main(){
 World world; Probe renderer(&world); OrthographicCamera camera(Point3D(0,0,10),Point3D(0,0,0),16,16,&renderer,2,100); world.m_camera=&camera;
 DirectionalLight red(RGBColor(0.4,0,0),Vector3D(0,0,-1)),blue(RGBColor(0,0,0.3),Vector3D(0,0,-1));
 Fragment f(Point3D(0,0,0),RGBColor(1),RGBColor(0.5),RGBColor(0),Vector3D(0,0,1));
 auto a=Material::shade({&red,&blue},camera,f);auto b=Material::shade({&blue,&red},camera,f);
 std::printf("ORDER_RB %.6f %.6f %.6f\nORDER_BR %.6f %.6f %.6f\n",a.r,a.g,a.b,b.r,b.g,b.b);
 if(!std::isfinite(a.r)||!std::isfinite(b.b))return 2;
 world.m_lights={&red};renderer.render(false,true);std::printf("MAPS_FIRST %zu\n",renderer.maps());
 renderer.render(false,true);std::printf("MAPS_REPEAT %zu\n",renderer.maps());
 renderer.render(false,false);std::printf("MAPS_DISABLED %zu\n",renderer.maps());
 std::puts("PRODUCTION_BASELINE_OK");return 0;
}
