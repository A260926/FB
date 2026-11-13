#include <bvh/v2/binned_sah_builder.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/stream.h>
#include <sstream>
#include <iostream>
#include <set>
#include <cmath>
#include <stdexcept>
using N=bvh::v2::Node<double,3>;
using V=bvh::v2::Vec<double,3>;
using T=bvh::v2::Tri<double,3>;
using B=bvh::v2::BBox<double,3>;
using Tree=bvh::v2::Bvh<N>;
struct Hit {size_t id=size_t(-1), tests=0; double t=100;};
Hit trace(const Tree& tree,const std::vector<T>& tris,V org) {
 bvh::v2::Ray<double,3> r(org,V(0,0,1),0,100);
 bvh::v2::GrowingStack<Tree::Index> stack; Hit h;
 tree.intersect<false,true>(r,tree.get_root().index,stack,[&](size_t a,size_t b){
  for(size_t i=a;i<b;++i){++h.tests; size_t id=tree.prim_ids[i];
   auto q=bvh::v2::PrecomputedTri<double>(tris[id]).intersect(r);
   if(q){r.tmax=std::get<0>(*q); h.t=r.tmax; h.id=id;}
  } return h.id!=size_t(-1);
 }); return h;
}
int main(){
 std::vector<T> tris; std::vector<B> boxes; std::vector<V> centers;
 for(size_t i=0;i<32;++i){double z=1+i/64.0;
  tris.emplace_back(V(-10,-10,z),V(10,10,z),V(10,9.5,z));
  boxes.push_back(tris.back().get_bbox()); centers.push_back(tris.back().get_center());
 }
 auto tree=bvh::v2::BinnedSahBuilder<N,16>::build(boxes,centers);
 auto hit=trace(tree,tris,V(0,-0.125,0));
 if(hit.id!=0||std::abs(hit.t-1)>1e-12) throw std::runtime_error("nearest triangle");
 size_t wasted=0; for(int i=0;i<8;++i){auto m=trace(tree,tris,V(-5,2+i*.5,0));
  if(m.id!=size_t(-1)) throw std::runtime_error("miss ray"); wasted+=m.tests;}
 std::set<size_t> ids(tree.prim_ids.begin(),tree.prim_ids.end());
 if(ids.size()!=32||tree.prim_ids.size()!=32||wasted==0) throw std::runtime_error("object references");
 std::stringstream bytes; bvh::v2::StdOutputStream out(bytes); tree.serialize(out);
 bvh::v2::StdInputStream in(bytes); auto restored=Tree::deserialize(in);
 if(!(restored==tree)||trace(restored,tris,V(0,-0.125,0)).id!=0) throw std::runtime_error("serialize");
 std::cout<<"OBJECT_BVH_OK triangles=32 refs="<<tree.prim_ids.size()<<" nearest=0 t="<<hit.t<<"\n";
 std::cout<<"OBJECT_OVERLAP miss_rays=8 triangle_tests="<<wasted<<"\n";
 std::cout<<"OBJECT_SERIALIZATION_OK\n";
}
