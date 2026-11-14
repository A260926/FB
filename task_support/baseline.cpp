#include "tests/gtest/gtest.h"
#include "accelerators/bvh.h"
#include "integrator.h"
#include "integrators/volpath.h"
#include "lights/point.h"
#include "lights/diffuse.h"
#include "media/homogeneous.h"
#include "shapes/triangle.h"
#include "primitive.h"
#include "scene.h"
#include "reflection.h"
#include "memory.h"
#include "samplers/random.h"
#include <cmath>
#include <cstdio>
using namespace pbrt;
namespace {
class FixedSampler : public Sampler {
public:
 FixedSampler():Sampler(1){}
 Float Get1D() override { return .25f; }
 Point2f Get2D() override { return Point2f(.25f,.375f); }
 std::unique_ptr<Sampler> Clone(int) override {return std::unique_ptr<Sampler>(new FixedSampler);}
};
}
TEST(SteadyBaseline, DirectAndMedium) {
 auto bvh=std::make_shared<BVHAccel>(std::vector<std::shared_ptr<Primitive>>{});
 auto point=std::make_shared<PointLight>(Translate(Vector3f(0,0,2)),MediumInterface(),Spectrum(8));
 Scene scene(bvh,{point}); FixedSampler sampler; MemoryArena arena;
 SurfaceInteraction ref;ref.p=Point3f(0,0,0);ref.n=Normal3f(0,0,1);ref.shading.n=ref.n;
 ref.wo=Vector3f(0,0,1);ref.dpdu=Vector3f(1,0,0);ref.dpdv=Vector3f(0,1,0);ref.shading.dpdu=ref.dpdu;ref.shading.dpdv=ref.dpdv;
 ref.bsdf=ARENA_ALLOC(arena,BSDF)(ref);ref.bsdf->Add(ARENA_ALLOC(arena,LambertianReflection)(Spectrum(.8f)));
 Spectrum direct=EstimateDirect(ref,Point2f(.2f,.4f),*point,Point2f(.3f,.7f),scene,sampler,arena,true);
 EXPECT_NEAR(.8*2/Pi,direct[0],2e-6);
 HomogeneousMedium medium(Spectrum(.1f),Spectrum(.4f),0);
 Ray ray(Point3f(),Vector3f(0,0,2),1,.37f,&medium);
 Spectrum tr=medium.Tr(ray,sampler);EXPECT_NEAR(std::exp(-1.),tr[0],1e-6);
 MediumInteraction mi;Spectrum beta=medium.Sample(ray,sampler,arena,&mi);
 ASSERT_TRUE(mi.IsValid());EXPECT_NEAR(-std::log(.75)/.5,mi.p.z,1e-6);
 EXPECT_NEAR(.8,beta[0],1e-6);EXPECT_FLOAT_EQ(.37f,mi.time);
 std::printf("PBRT_STEADY_MEDIUM_OK direct=%.8f Tr=%.8f scatter_z=%.8f beta=%.8f\n",double(direct[0]),double(tr[0]),double(mi.p.z),double(beta[0]));
}
TEST(SteadyBaseline, VolpathEmitter) {
 Transform id;int idx[]={0,1,2};Point3f p[]={{0,0,2},{0,2,2},{2,0,2}};
 auto shapes=CreateTriangleMesh(&id,&id,false,1,idx,3,p,nullptr,nullptr,nullptr,nullptr,nullptr);
 ASSERT_EQ(1u,shapes.size());
 auto lamp=std::make_shared<DiffuseAreaLight>(id,MediumInterface(),Spectrum(3),1,shapes[0]);
 auto prim=std::make_shared<GeometricPrimitive>(shapes[0],nullptr,lamp,MediumInterface());
 auto bvh=std::make_shared<BVHAccel>(std::vector<std::shared_ptr<Primitive>>{prim});Scene scene(bvh,{lamp});
 auto sampler=std::make_shared<FixedSampler>();MemoryArena arena;
 VolPathIntegrator integrator(0,nullptr,sampler,Bounds2i(Point2i(0,0),Point2i(1,1)),1,"uniform");
 integrator.Preprocess(scene,*sampler);
 Spectrum L=integrator.Li(RayDifferential(Ray(Point3f(.25f,.25f,0),Vector3f(0,0,2),Infinity,.37f)),scene,*sampler,arena,0);
 EXPECT_FLOAT_EQ(3,L[0]);EXPECT_FLOAT_EQ(3,L[1]);EXPECT_FLOAT_EQ(3,L[2]);
 std::printf("PBRT_STEADY_VOLPATH_OK rgb=3,3,3\n");
}
