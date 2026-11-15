#include "src/render.h"
#include "src/util.h"
#include <cstdio>
#include <cmath>
#include <memory>
extern Vec3 PathTrace(const Scene&,const Vec3&,const Vec3&,float,int,Random&);
int main(){
    Mesh* m=CreateQuadMesh(2.0f); m->RebuildBVH();
    Scene s; s.sky.horizon=s.sky.zenith=Vec3(0.0f);
    Primitive p; p.type=eMesh; p.mesh=GeometryFromMesh(m);
    p.material.color=Vec3(.3f,.5f,.7f); p.material.emission=Vec3(2,3,4);
    p.lightSamples=1; s.AddPrimitive(p); s.Build();
    float t=0; Vec3 n;
    if(!PrimitiveIntersect(s.primitives[0],Ray(Vec3(.25f,2,.1f),Vec3(0,-1,0),.6f),t,&n))return 1;
    if(std::abs(t-2)>1e-5 || std::abs(m->area-16)>1e-5)return 2;
    Random rnd(123); Vec3 L=PathTrace(s,Vec3(.25f,2,.1f),Vec3(0,-1,0),.6f,3,rnd);
    if(Length(L-Vec3(2,3,4))>1e-5)return 3;
    Options o={}; o.mode=ePathTrace; o.width=o.height=4;o.maxDepth=3;o.clamp=100;
    o.filter=Filter(eFilterBox,.5f); Color pixels[16]={};
    Camera cam;cam.position=Vec3(0,2,0); cam.rotation=Quat(-.70710678f,0,0,.70710678f);
    std::unique_ptr<Renderer> renderer(CreateCpuRenderer(&s)); renderer->Render(cam,o,pixels);
    int lit=0;for(const auto& v:pixels){if(v.x>0)++lit;if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))return 4;}
    if(lit<4)return 5;
    std::printf("BASELINE_MESH area=%.6f hit=%.6f rgb=%.6f,%.6f,%.6f cpu_pixels=16\n",m->area,t,L.x,L.y,L.z);
    delete[] s.bvh.nodes;s.bvh.nodes=nullptr;delete m;return 0;
}
