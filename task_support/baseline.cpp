#include "Renderer.h"
#include "Camera.h"
#include "Scene.h"
#include <Canvas.h>
#include <iostream>
#include <memory>
#include <cstdint>
int main(){
 std::shared_ptr<TextureMap> tex=std::make_shared<TextureMap>();tex->width=2;tex->height=2;tex->pixels={0xffcc3322u,0xff22cc33u,0xff3333ccu,0xffdddd99u};
 ModelTriangle a({-3,-2,0},{3,-2,0},{3,2,0},Colour(180,120,80));
 ModelTriangle b({-3,-2,0},{3,2,0},{-3,2,0},Colour(180,120,80));
 a.normal=b.normal={0,0,1};a.vertexNormals=b.vertexNormals={glm::vec3(0,0,1),glm::vec3(0,0,1),glm::vec3(0,0,1)};
 a.texture=b.texture=tex;a.texturePoints={TexturePoint(0,1),TexturePoint(1,1),TexturePoint(1,0)};b.texturePoints={TexturePoint(0,1),TexturePoint(1,0),TexturePoint(0,0)};
 std::vector<ModelTriangle> mesh={a,b};Primitives prim;Scene s(mesh,prim);
 auto h=s.intersect({0,0,4},{0,0,-1});if(!h.hit||!h.intersectedTriangle.texture||h.intersectedTriangle.material!=Material::Diffuse)return 1;
 Camera camera(32,24,2.0f,{0,0,4});camera.scale=8.0f;Canvas canvas(32,24);Light light;light.position={0,1,2};light.intensity=16.0f;
 renderPathTraced(mesh,camera,canvas,4,{light});
 uint64_t hash=1469598103934665603ULL,sum=0;for(auto p:canvas.pixels){for(unsigned shift:{0u,8u,16u}){auto c=(p>>shift)&255u;sum+=c;hash^=c;hash*=1099511628211ULL;}}
 uint32_t center=canvas.getPixelColour(16,12);if(sum==0 || (center&0xffffffu)==0 || canvas.pixels.size()!=768)return 2;
 std::cout<<"PATH_RENDER size=32x24 spp=4 hit_texture=1 sum="<<sum<<" hash="<<hash<<" center="<<std::hex<<center<<std::dec<<"\n";
 std::cout<<"BASELINE_COMPLETE textured_path_production\n";
}
