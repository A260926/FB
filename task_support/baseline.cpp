#include "task_support/native.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void require(bool b){if(!b)std::abort();}
int main(){
 ParamSet p;int indices[]={0,1,2};
 Point points[]={Point(-1,-1,2),Point(2,-1,2),Point(0,2,3)};
 float uv[]={0,0,2,0,0,3};p.AddInt("indices",indices,3);p.AddPoint("P",points,3);p.AddFloat("uv",uv,6);
 Reference<Shape> mesh=CreateTriangleMeshShape(Transform(),false,p);require(bool(mesh));
 vector<Reference<Shape>> triangles;mesh->Refine(triangles);require(triangles.size()==1);
 ParamSet mp;std::map<string,Reference<Texture<float>>> ft;std::map<string,Reference<Texture<Spectrum>>> st;
 TextureParams textures(mp,mp,ft,st);
 Reference<Texture<float>> scalar=CreatefbmFloatTexture(Transform(),textures);
 Reference<Texture<Spectrum>> color=CreatefbmSpectrumTexture(Transform(),textures);
 string wood="wood";st[wood]=color;mp.AddTexture("Kd",wood);
 TextureParams materialParams(mp,mp,ft,st);
 Reference<Material> matte=CreateMatteMaterial(Transform(),materialParams);
 Reference<Primitive> primitive=new GeometricPrimitive(triangles[0],matte,nullptr);
 Transform xform=Translate(Vector(2,0,0))*Scale(1.3,0.8,1.1);
 Reference<Primitive> instance=new InstancePrimitive(primitive,xform);
 Point q=0.3f*points[0]+0.4f*points[1]+0.3f*points[2];
 Point world=xform(q);Vector direction=xform(Vector(0,0,1));
 RayDifferential ray(Ray(world-2.f*direction,direction,0,5));
 ray.hasDifferentials=true;ray.rx=Ray(ray.o+Vector(.03,0,0),ray.d);ray.ry=Ray(ray.o+Vector(0,.02,0),ray.d);
 Intersection is;require(instance->Intersect(ray,&is));
 BSDF* bsdf=is.GetBSDF(ray);require(bsdf!=nullptr);
 float f=scalar->Evaluate(is.dg);Spectrum s=color->Evaluate(is.dg);require(std::isfinite(f));
 require(std::abs(s.y()-Spectrum(f).y())<1e-6f);
 Vector dx,dy;
 // The mapping's parameter is used directly by the existing implementation.
 IdentityMapping3D identity{Transform()};Point mapped=identity.Map(is.dg,&dx,&dy);
 require(Distance(mapped,is.dg.p)<1e-6f);require(dx.Length()>0 && dy.Length()>0);
 Vector n(is.dg.nn);Spectrum response=bsdf->f(n,n);require(std::isfinite(response.y()));require(std::abs(response.y()-Spectrum(f).Clamp().y()*INV_PI)<2e-6f);
 std::printf("PBRT_TEXTURE_PRODUCTION_OK hit=(%.6f,%.6f,%.6f) fbm=%.6f dx=%.6f dy=%.6f brdf=%.6f\n",mapped.x,mapped.y,mapped.z,f,dx.Length(),dy.Length(),response.y());
 BSDF::FreeAll();
}
