#include "Hittables/Mesh.hpp"
#include "Hittables/MeshInstance.hpp"
#include "Materials/Emissive.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>
int main(){
 auto material=std::make_shared<Emissive>(Color(1,1,1));
 std::vector<Triangle> triangles;
 triangles.emplace_back(Vector3(0,0,0),Vector3(1,0,0),Vector3(0,1,0),material);
 auto mesh=std::make_shared<Mesh>(Vector3(),material,triangles);
 auto instance=std::make_shared<MeshInstance>(mesh,Vector3(1,2,3),Vector3(0,0,90),Vector3(2,1,1),material);
 BVHNode world(std::vector<std::shared_ptr<Hittable>>{instance});
 Ray ray(Vector3(.75,2.5,4),Vector3(0,0,-1));HitRecord hit;
 bool found=world.hit(ray,hit,.001,100),any=world.hitAny(ray,.001,100);
 double pdf=instance->pdfValue(ray.getOrigin(),ray.getDirection());
 HittableLightSample sample;bool sampled=instance->sampleLight(ray.getOrigin(),sample);
 AABB box;bool bounded=instance->createBoundingBox(box);
 if(!found||!any||!bounded||!sampled||!sample.valid||std::abs(hit.t0-1)>1e-8||std::abs(pdf-1)>1e-8)return 1;
 std::cout<<"BASELINE_OK nearest="<<hit.t0<<" any="<<any<<" area="<<mesh->surfaceArea()<<" instance_pdf="<<pdf<<" bounded="<<bounded<<" sampled="<<sample.valid<<"\n";
 std::cout<<"mesh_updates=absent geometry_cache=construction_only\n";
}
