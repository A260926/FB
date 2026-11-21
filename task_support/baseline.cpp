#include "Scene/Scene.hpp"
#include "Hittables/Sphere.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/Emissive.hpp"
#include "Materials/Lambertian.hpp"
#include "Sampler.hpp"
#include "src/renderer/RendererInternal.hpp"
#include <cmath>
#include <iostream>
#include <memory>
static bool near(double a,double b){return std::abs(a-b)<1e-10;}
int main(){
 Scene scene; scene.setRenderSky(SKY_NONE); scene.setBackgroundColor(Color(.1,.2,.3));
 scene.setMaxLightBounces(2);
 auto lamp=std::make_shared<Emissive>(Color(.7,.4,.2));
 scene.addHittable(std::make_shared<Sphere>(Vector3(0,0,-3),1,lamp));
 Ray r(Vector3(0,0,0),Vector3(0,0,-1)); HitRecord hit;
 if(!Renderer::internal::_checkHits(scene,r,hit)||!near(hit.t0,2)) return 1;
 Sampler::setRenderSeed(42); Sampler::beginPixelSample(0,0,0);
 Color c=Renderer::internal::_calculateLightRaysColor(r,scene); Sampler::endPixelSample();
 if(!near(c.getRed(),.7)||!near(c.getGreen(),.4)||!near(c.getBlue(),.2))return 2;
 Ray miss(Vector3(0,0,0),Vector3(0,1,0));
 Color sky=Renderer::internal::_calculateLightRaysColor(miss,scene);
 if(!near(sky.getRed(),.1)||!near(sky.getBlue(),.3))return 3;
 auto mat=std::make_shared<Lambertian>(Color(.8,.5,.2));
 Triangle tri(Vector3(-2,-2,-1),Vector3(2,-2,-1),Vector3(0,2,-1),mat);
 HitRecord th;if(!tri.hit(r,th,.001,10))return 4;
 ScatterRecord sr;if(!mat->scatter(r,th,sr)||!near(sr.attenuation.getRed(),.8))return 5;
 double p=mat->scatteringPDF(r,th,Vector3(0,0,1));if(!near(p,1/std::acos(-1.0)))return 6;
 std::cout<<"LUZ_BASELINE hit_t="<<hit.t0<<" emission="<<c.getRed()<<","<<c.getGreen()<<","<<c.getBlue()<<" sky="<<sky.getRed()<<","<<sky.getGreen()<<","<<sky.getBlue()<<" cosine_pdf="<<p<<"\n";
}
