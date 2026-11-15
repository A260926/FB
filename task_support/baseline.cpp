#include "scene_api.hpp"
#include <cstdio>
using namespace tinyscene;
static bool hit(float x) {
 tinybvh::Ray r(tinybvh::bvhvec3(x,0,0),tinybvh::bvhvec3(0,0,1));
 Scene::tlas->Intersect(r); return r.hit.t>3.9f && r.hit.t<4.1f;
}
int main() {
 int mat=Scene::AddMaterial(ts_vec3(.4f),"normal"),m=Scene::AddMesh(0);
 Scene::AddTriToMesh(m,ts_vec3(-1,-1,4),ts_vec3(1,-1,4),ts_vec3(0,1,4),mat);
 Mesh* mesh=Scene::meshPool[m]; mesh->BuildMaterialList();
 ts_mat4 a,b; b[3]=8;
 int left=Scene::AddNode(new Node(m,a)),right=Scene::AddNode(new Node(m,b));
 Scene::AddInstance(left);Scene::AddInstance(right);Scene::UpdateSceneGraph(0);
 if(!hit(0)||!hit(8)||hit(4))return 2;
 tinybvh::Ray shadow(tinybvh::bvhvec3(0,0,0),tinybvh::bvhvec3(0,0,1),3);
 if(Scene::tlas->IsOccluded(shadow))return 3;
 shadow.hit.t=5;if(!Scene::tlas->IsOccluded(shadow))return 4;
 printf("BASELINE_SHARED instances=%zu left=1 right=1 finite_shadow=1\n",Scene::instPool.size());
 Mesh::Pose base,delta;
 for(auto v:mesh->vertices){base.positions.push_back(ts_vec3(v));base.normals.push_back(ts_vec3(0,0,1));delta.positions.push_back(ts_vec3(4,0,0));delta.normals.push_back(ts_vec3(0));}
 mesh->poses={base,delta};
 Scene::nodePool[left]->weights={0};Scene::nodePool[left]->morphed=true;
 Scene::nodePool[right]->weights={.5f};Scene::nodePool[right]->morphed=true;
 Scene::UpdateSceneGraph(0);
 printf("STARTING_MORPH source_first_x=%.1f expected_rest=-1.0 left_hit=%d shifted_right_hit=%d\n",mesh->vertices[0].x,hit(0),hit(10));
 return 0;
}
