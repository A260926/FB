#include <bvh/v2/default_builder.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/stream.h>
#include <sstream>
#include <iostream>
#include <cmath>
using V=bvh::v2::Vec<float,3>; using N=bvh::v2::Node<float,3>;
using B=bvh::v2::Bvh<N>; using T=bvh::v2::Tri<float,3>; using P=bvh::v2::PrecomputedTri<float>;
static bool query(const B& b,const std::vector<T>& t,unsigned id,float z) {
  bvh::v2::Ray<float,3> ray{V(float(id)*3+.2f,.2f,0),V(0,0,2),0,100};
  size_t hit=size_t(-1);bvh::v2::GrowingStack<N::Index> stack;
  b.intersect<false,true>(ray,b.get_root().index,stack,[&](size_t begin,size_t end){
    for(auto i=begin;i<end;++i)if(auto h=P(t[b.prim_ids[i]]).intersect(ray)) {hit=b.prim_ids[i];ray.tmax=std::get<0>(*h);}return hit!=size_t(-1);
  }); return hit==id&&std::abs(ray.tmax-z/2)<1e-5;
}
int main(){
  std::vector<T> t; std::vector<bvh::v2::BBox<float,3>> boxes; std::vector<V> centers;
  for(unsigned i=0;i<64;++i){float x=float(i)*3;t.emplace_back(V(x,0,4),V(x+1,0,4),V(x,1,4));boxes.push_back(t.back().get_bbox());centers.push_back(t.back().get_center());}
  auto b=bvh::v2::DefaultBuilder<N>::build(boxes,centers);
  for(unsigned i=0;i<64;++i)if(!query(b,t,i,4))return 1;
  std::stringstream ss(std::ios::in|std::ios::out|std::ios::binary);bvh::v2::StdOutputStream out(ss);b.serialize(out);
  bvh::v2::StdInputStream in(ss);auto loaded=B::deserialize(in);
  for(unsigned i=0;i<64;++i)if(!query(loaded,t,i,4))return 2;
  for(auto& tri:t){tri.p0[2]+=3;tri.p1[2]+=3;tri.p2[2]+=3;}
  loaded.refit([&](N& n){auto box=bvh::v2::BBox<float,3>::make_empty();for(size_t i=n.index.first_id();i<n.index.first_id()+n.index.prim_count();++i)box.extend(t[loaded.prim_ids[i]].get_bbox());n.set_bbox(box);});
  bvh::v2::ReinsertionOptimizer<N>::optimize(loaded);
  for(unsigned i=0;i<64;++i)if(!query(loaded,t,i,7))return 3;
  auto child=loaded.extract_bvh(loaded.get_root().index.first_id());
  for(auto id:child.prim_ids)if(!query(child,t,unsigned(id),7))return 4;
  bvh::v2::Ray<float,3> shortRay{V(.2f,.2f,0),V(0,0,2),0,3};bool occluded=false;bvh::v2::GrowingStack<N::Index> stack;
  loaded.intersect<true,false>(shortRay,loaded.get_root().index,stack,[&](size_t begin,size_t end){for(auto i=begin;i<end;++i)if(P(t[loaded.prim_ids[i]]).intersect(shortRay))return occluded=true;return false;});
  if(occluded)return 5;
  std::cout<<"BASELINE_OK primary=64 restored=64 refit=64 subtree="<<child.prim_ids.size()<<" finite_shadow=clear stream_bytes="<<ss.str().size()<<"\n";
}
