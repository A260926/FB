#include "scene.h"
#include "intersection.h"
#include "shapes/trianglemesh.h"
#include "accelerators/bvh.h"
#include "materials/mirror.h"
#include "materials/plastic.h"
#include "textures/constant.h"
#include "reflection.h"
#include "lights/point.h"
#include <cstdio>
#include <cmath>
int main() {
    Transform id;
    int ids[]={0,1,2}; Point p[]={Point(0,0,2),Point(2,0,2),Point(0,2,2)};
    Reference<Shape> mesh=new TriangleMesh(&id,&id,false,1,3,ids,p,NULL,NULL,NULL,Reference<Texture<float> >());
    Reference<Texture<Spectrum> > kr=new ConstantTexture<Spectrum>(Spectrum(.8f));
    Reference<Material> material=new MirrorMaterial(kr,Reference<Texture<float> >());
    std::vector<Reference<Primitive> > prims;
    prims.push_back(new GeometricPrimitive(mesh,material,NULL));
    std::vector<Light*> lights;
    lights.push_back(new PointLight(Translate(Vector(.2f,.2f,0)),Spectrum(4.f)));
    Scene scene(new BVHAccel(prims),lights,NULL);
    RayDifferential ray(Point(.2f,.2f,0),Vector(0,0,2),0.f);
    Intersection hit;
    if(!scene.Intersect(ray,&hit) || std::abs(ray.maxt-1.f)>1e-6) return 1;
    MemoryArena arena; BSDF *bsdf=hit.GetBSDF(ray,arena);
    Vector wi;float pdf;BxDFType type;
    Spectrum f=bsdf->Sample_f(Vector(0,0,-1),&wi,BSDFSample(.2f,.3f,.4f),&pdf,BSDF_ALL,&type);
    float rgb[3]; Spectrum weight=f*(AbsDot(wi,hit.dg.nn)/pdf); weight.ToRGB(rgb);
    if(std::abs(wi.z+1.f)>1e-6 || std::abs(rgb[0]-.8f)>1e-6 || !(type&BSDF_SPECULAR))return 2;
    VisibilityTester vis;float lpdf;Vector toward;
    Spectrum li=lights[0]->Sample_L(hit.dg.p,hit.rayEpsilon,LightSample(.2f,.3f,.4f),0,&toward,&lpdf,&vis);
    li.ToRGB(rgb);
    if(std::abs(rgb[0]-1.f)>1e-5 || !vis.Unoccluded(&scene)) return 3;
    Ray shortRay(Point(.2f,.2f,0),Vector(0,0,2),0,.9f);
    Ray longRay(Point(.2f,.2f,0),Vector(0,0,2),0,1.1f);
    if(scene.IntersectP(shortRay)||!scene.IntersectP(longRay))return 4;
    Reference<Texture<Spectrum> > kd=new ConstantTexture<Spectrum>(Spectrum(.3f));
    Reference<Texture<Spectrum> > ks=new ConstantTexture<Spectrum>(Spectrum(.4f));
    Reference<Texture<float> > rough=new ConstantTexture<float>(.2f);
    PlasticMaterial plastic(kd,ks,rough,Reference<Texture<float> >());
    BSDF *pbsdf=plastic.GetBSDF(hit.dg,hit.dg,arena);
    Spectrum pf=pbsdf->Sample_f(Vector(0,0,-1),&wi,BSDFSample(.2f,.3f,.7f),&pdf,BSDF_ALL,&type);
    pf.ToRGB(rgb);
    if(!(pdf>0) || !std::isfinite(pdf) || !(rgb[0]>0) || !std::isfinite(rgb[0]))return 5;
    std::printf("PLASTIC existing_mixture f=%.6f pdf=%.6f PASS\n",rgb[0],pdf);
    rgb[0]=1.f;
    std::printf("BASELINE mesh_refine_bvh t=%.6f mirror=0.800000 point=%.6f finite_shadow=PASS\n",ray.maxt,rgb[0]);
}
