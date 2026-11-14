#include <bvh/v2/default_builder.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/stack.h>
#include <sstream>
#include <iostream>
#include <cmath>
using V=bvh::v2::Vec<float,3>;using N=bvh::v2::Node<float,3>;
using B=bvh::v2::Bvh<N>;using T=bvh::v2::Tri<float,3>;using Box=bvh::v2::BBox<float,3>;
static std::pair<size_t,float> trace(const B& tree,const std::vector<T>& tris){
 bvh::v2::Ray<float,3> ray{V(0,0,0),V(0,0,2),0,20};
 bvh::v2::SmallStack<N::Index,64> stack;size_t id=SIZE_MAX;
 tree.intersect<false,true>(ray,tree.get_root().index,stack,[&](size_t begin,size_t end){
  bool hit=false;for(size_t i=begin;i<end;++i){size_t j=tree.prim_ids[i];
   if(auto h=bvh::v2::PrecomputedTri<float>(tris[j]).intersect(ray)){
    id=j;ray.tmax=std::get<0>(*h);hit=true;
   }
  }return hit;
 });return {id,ray.tmax};
}
int main(){
 std::vector<T> tris{{V(-1,-1,2),V(1,-1,2),V(0,1,2)},
 {V(-1,-1,4),V(1,-1,4),V(0,1,4)},
 {V(9,-1,1),V(11,-1,1),V(10,1,1)}};
 std::vector<Box> boxes;std::vector<V> centers;
 for(auto& t:tris){boxes.push_back(t.get_bbox());centers.push_back(t.get_center());}
 bvh::v2::DefaultBuilder<N>::Config cfg;cfg.quality=bvh::v2::DefaultBuilder<N>::Quality::Low;cfg.max_leaf_size=1;
 B tree=bvh::v2::DefaultBuilder<N>::build(boxes,centers,cfg);
 if(trace(tree,tris)!=std::pair<size_t,float>{0,1})return 1;
 for(auto* v:{&tris[0].p0,&tris[0].p1,&tris[0].p2})(*v)[2]=6;
 tree.refit([&](N& n){Box box=Box::make_empty();
  for(size_t i=n.index.first_id();i<n.index.first_id()+n.index.prim_count();++i)box.extend(tris[tree.prim_ids[i]].get_bbox());n.set_bbox(box);
 });
 std::stringstream bytes(std::ios::in|std::ios::out|std::ios::binary);
 bvh::v2::StdOutputStream out(bytes);tree.serialize(out);bytes.seekg(0);
 bvh::v2::StdInputStream in(bytes);B restored=B::deserialize(in);
 auto [id,t]=trace(restored,tris);
 if(id!=1||std::abs(t-2)>1e-6||!(tree==restored))return 2;
 std::cout<<"BASELINE_NATIVE_REFIT_OK id="<<id<<" t="<<t<<" nodes="<<tree.nodes.size()<<"\n";
}
