#include "src/render.h"
#include "src/util.h"
#include <memory>
#include <iostream>
int main() {
 Scene scene;
 scene.sky.horizon=scene.sky.zenith=Vec3(0.0f);
 Primitive sphere;
 sphere.type=eSphere; sphere.sphere.radius=1.0f;
 sphere.startTransform=Transform(Vec3(0,0,-3)); sphere.endTransform=sphere.startTransform;
 sphere.material.emission=Vec3(2,1,0.5f); sphere.lightSamples=0;
 scene.AddPrimitive(sphere); scene.Build();
 float t=0;Vec3 n;
 if(!PrimitiveIntersect(scene.primitives[0],Ray(Vec3(0),Vec3(0,0,-1),0.5f),t,&n)||fabs(t-2)>1e-5f||n.z<0.99f)return 2;
 Camera camera;camera.position=Vec3(0);camera.rotation=Quat();camera.fov=DegToRad(35.0f);
 Options o{};o.width=o.height=12;o.mode=ePathTrace;o.maxDepth=2;o.maxSamples=1;o.clamp=100;o.filter=Filter(eFilterBox,0.0f);
 std::vector<Color> pixels(144,Color(0.0f));
 std::unique_ptr<Renderer> renderer(CreateCpuRenderer(&scene));renderer->Render(camera,o,pixels.data());
 unsigned lit=0;double sum=0;for(const auto& c:pixels){if(!std::isfinite(c.x)||!std::isfinite(c.y)||!std::isfinite(c.z))return 3;if(c.x>0)++lit;sum+=c.x;}
 if(lit<20||sum<=0)return 4;
 o.mode=eNormals;std::fill(pixels.begin(),pixels.end(),Color(0.0f));renderer->Render(camera,o,pixels.data());
 if(pixels[6*12+6].z<0.9f)return 5;
 std::cout<<"SPHERE_HIT t="<<t<<" normal_z="<<n.z<<"\nCPU_PATH pixels=144 lit="<<lit<<" emission_sum="<<sum<<"\nCPU_NORMAL center_z="<<pixels[78].z<<"\nBASELINE_COMPLETE\n";
 delete[] scene.bvh.nodes;scene.bvh.nodes=nullptr;
}
