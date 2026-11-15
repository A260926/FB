#include "OBJReader.hpp"
#include "Hittables/MeshInstance.hpp"
#include "Materials/Emissive.hpp"
#include "Scene/Scene.hpp"
#include "Utilities.hpp"
#include "Sampler.hpp"
#include <fstream>
#include <iostream>
#include <cmath>
#define REQUIRE(x) do { if(!(x)){std::cerr << "baseline failure " << __LINE__ << "\n";return 1;} }while(0)
int main(){
 std::ofstream f(".task-build/base.obj");
 f << "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n";f.close();
 ObjReadOptions opts;opts.quiet=true;
 auto data=readObjMeshData(".task-build/base.obj",opts);
 REQUIRE(data.triangles.size()==1);
 auto light=std::make_shared<Emissive>(Color(2,2,2));
 auto mesh=std::make_shared<Mesh>(buildObjMesh(data,Vector3(),Vector3(),Vector3(1,1,1),light));
 auto inst=std::make_shared<MeshInstance>(mesh,Vector3(0,0,3),Vector3(),Vector3(2,1,1),light);
 Ray r(Vector3(0,0,0),Vector3(0,0,1));HitRecord h;
 REQUIRE(inst->hit(r,h,0.001,100));REQUIRE(std::fabs(h.t0-3)<1e-8);REQUIRE(h.material==light.get());
 REQUIRE(inst->hitAny(r,0.001,4));REQUIRE(!inst->hitAny(r,0.001,2));
 REQUIRE(std::fabs(mesh->surfaceArea()-2)<1e-9);
 REQUIRE(std::fabs(inst->lightSelectionWeight()-8)<1e-8);
 Sampler::setRenderSeed(42);Sampler::beginPixelSample(0,0,0);
 HittableLightSample s;REQUIRE(inst->sampleLight(Vector3(),s));REQUIRE(s.valid&&s.pdf>0&&s.material==light.get());
 REQUIRE(std::fabs(inst->pdfValue(Vector3(),s.direction)-s.pdf)<1e-7);
 HittableEmissionSample e;REQUIRE(inst->sampleEmission(e));REQUIRE(e.valid&&e.powerScale>0);Sampler::endPixelSample();
 Scene scene;scene.addHittable(inst);scene.updateLights();REQUIRE(scene.getLights().size()==1);
 std::cout << "BASELINE_MESH_OK triangles=1 area=2 instance_weight=8 hit_t=3 light_count=1\n";
}
