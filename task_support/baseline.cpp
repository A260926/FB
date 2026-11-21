#include "api.h"
#include "sampling.h"
#include "lights/infinite.h"
#include "shapes/sphere.h"
#include "accelerators/bvh.h"
#include "primitive.h"
#include "scene.h"
#include <cmath>
#include <iostream>
using namespace pbrt;
int main() {
    Options o; o.nThreads=1; pbrtInit(o);
    Transform identity;
    auto shape=std::make_shared<Sphere>(&identity,&identity,false,1,-1,1,360);
    std::vector<std::shared_ptr<Primitive>> p;
    p.push_back(std::make_shared<GeometricPrimitive>(shape,nullptr,nullptr,MediumInterface()));
    auto light=std::make_shared<InfiniteAreaLight>(identity,Spectrum(2),1,"");
    Scene scene(std::make_shared<BVHAccel>(p),{light});
    SurfaceInteraction hit;
    if(!scene.Intersect(Ray(Point3f(0,0,-4),Vector3f(0,0,1)),&hit)) return 2;
    Interaction ref(Point3f(0,0,-4),0,MediumInterface());
    double sum=0;
    for(int i=0;i<32;++i) {
        Point2f u((i+.5f)/32, .13f+.73f*(i%7)/7);
        Vector3f wi; Float pdf; VisibilityTester vis;
        Spectrum li=light->Sample_Li(ref,u,&wi,&pdf,&vis);
        Float q=light->Pdf_Li(ref,wi);
        if(!std::isfinite(pdf)||pdf<=0||std::abs(q-pdf)>1e-5||std::abs(li[0]-2)>1e-5) return 3;
        if(std::abs(light->Le(RayDifferential(ref.p,wi))[0]-2)>1e-5) return 4;
        Ray ray; Normal3f n; Float pp,pd,qp,qd;
        light->Sample_Le(u,Point2f(.27f,.63f),.4f,&ray,&n,&pp,&pd);
        light->Pdf_Le(ray,n,&qp,&qd);
        if(std::abs(pp-qp)>1e-5||std::abs(pd-qd)>1e-5||ray.time!=.4f) return 5;
        sum+=li[0];
    }
    std::cout<<"INFINITE_BASELINE samples=32 radiance_sum="<<sum<<" sphere_hit_z="<<hit.p.z<<"\n";
    pbrtCleanup(); return 0;
}
