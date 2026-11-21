#include <bvh/v2/default_builder.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/stream.h>
#include <sstream>
#include <iostream>
#include <cmath>
using namespace bvh::v2;
using V=Vec<float,3>; using N=Node<float,3>; using T=Tri<float,3>;
int main(){
 std::vector<T> ts;std::vector<BBox<float,3>> boxes;std::vector<V> centers;
 for(int i=0;i<64;++i){float x=3.0f*i;ts.emplace_back(V(x-1,-1,3),V(x+1,-1,3),V(x,1,3));boxes.push_back(ts.back().get_bbox());centers.push_back(ts.back().get_center());}
 auto tree=DefaultBuilder<N>::build(boxes,centers);
 auto query=[&](float x,float z){Ray<float,3> ray{V(x,0,0),V(0,0,2),0,100};size_t id=SIZE_MAX;SmallStack<N::Index,128> st;
 tree.intersect<false,true>(ray,tree.get_root().index,st,[&](size_t b,size_t e){for(size_t i=b;i<e;++i){auto j=tree.prim_ids[i];if(auto h=PrecomputedTri<float>(ts[j]).intersect(ray)){id=j;ray.tmax=std::get<0>(*h);}}return id!=SIZE_MAX;});
 return id==21 && std::abs(ray.tmax-z/2)<1e-5;};
 if(!query(63,3))return 2;
 std::stringstream ss;StdOutputStream out(ss);tree.serialize(out);StdInputStream in(ss);auto loaded=Bvh<N>::deserialize(in);if(!(tree==loaded))return 3;
 for(auto& t:ts){t.p0[2]+=2;t.p1[2]+=2;t.p2[2]+=2;}
 tree.refit([&](N& n){auto b=BBox<float,3>::make_empty();for(size_t i=n.index.first_id();i<n.index.first_id()+n.index.prim_count();++i)b.extend(ts[tree.prim_ids[i]].get_bbox());n.set_bbox(b);});
 if(!query(63,5))return 4;
 std::cout<<"BASELINE triangles=64 original_id=21 t_before=1.5 t_after=2.5 archive_equal=1\n";
}
