#include "task_support/native.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void require(bool b){if(!b)std::abort();}
int main(){
 ParamSet p;
 int indices[]={0,1,2}; Point points[]={Point(-1,-1,2),Point(1,-1,2),Point(0,1,2)};
 p.AddInt("indices",indices,3);p.AddPoint("P",points,3);
 Reference<Shape> mesh=CreateTriangleMeshShape(Transform(),false,p);
 require(bool(mesh));
 vector<Reference<Shape>> triangles;mesh->Refine(triangles);require(triangles.size()==1);
 float t;DifferentialGeometry dg;Ray ray(Point(0,0,0),Vector(0,0,2),0.f,10.f,.75f);
 require(triangles[0]->Intersect(ray,&t,&dg));require(std::abs(t-1)<1e-6f);require(std::abs(dg.p.z-2)<1e-6f);
 Reference<Shape> emitter=triangles[0];
 ParamSet lp;
 std::map<string,Reference<Texture<float>>> ft;std::map<string,Reference<Texture<Spectrum>>> st;
 TextureParams textures(lp,lp,ft,st);Reference<Material> matte=CreateMatteMaterial(Transform(),textures);Light* light=CreateAreaLight(Transform(),lp,emitter);
 vector<Reference<Primitive>> objects;objects.push_back(new GeometricPrimitive(emitter,matte,static_cast<AreaLight*>(light)));
 vector<Light*> lights;lights.push_back(light);
 Scene scene(nullptr,CreateDirectIntegrator(ParamSet()),new TaskEmptyVolume(),new TaskFixedSampler(),CreateGridAccelerator(objects,ParamSet()),lights,nullptr);
 Intersection is;require(scene.Intersect(ray,&is));require(is.primitive->GetAreaLight()==light);
 Vector wi;float pdf;VisibilityTester visibility;
 Spectrum emission=light->Sample_L(Point(0,0,4),.25f,.5f,&wi,&pdf,&visibility);
 require(pdf>0 && std::isfinite(pdf));require(emission.y()>0);
 float back=light->Pdf(Point(0,0,4),wi);require(std::abs(back-pdf)<1e-5f);
 float alpha=0;RayDifferential primary(Ray(Point(0,0,4),Vector(0,0,-1)));
 Sample sample(scene.surfaceIntegrator,scene.volumeIntegrator,&scene);
 for(u_int i=0;i<sample.n1D.size();++i)for(u_int j=0;j<sample.n1D[i];++j)sample.oneD[i][j]=.5f;
 for(u_int i=0;i<sample.n2D.size();++i)for(u_int j=0;j<2*sample.n2D[i];++j)sample.twoD[i][j]=.5f;
 Spectrum radiance=scene.Li(primary,&sample,&alpha);require(radiance.y()>0);
 std::printf("STATIC_MESH t=%.6f area=%.6f pdf=%.6f radiance=%.6f\n",t,triangles[0]->Area(),pdf,radiance.y());
 std::puts("PBRT_STATIC_LIGHT_OK");
}
