// Baseline, MIT, calls unmodified builder, traversal and refit.
#include <bvh/v2/bvh.h>
#include <bvh/v2/binned_sah_builder.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/stack.h>
#include <cassert>
#include <cstdio>
using V=bvh::v2::Vec<double,3>;using N=bvh::v2::Node<double,3>;using B=bvh::v2::Bvh<N>;using T=bvh::v2::Tri<double,3>;
int main(){std::vector<T> ts;for(int i=0;i<16;++i){double x=3*i;ts.emplace_back(V(x,0,2),V(x+1,0,2),V(x,1,2));}std::vector<bvh::v2::BBox<double,3>> boxes;std::vector<V> centers;for(auto&t:ts){boxes.push_back(t.get_bbox());centers.push_back(t.get_center());}auto b=bvh::v2::BinnedSahBuilder<N>::build(boxes,centers);auto query=[&](double expected){bvh::v2::Ray<double,3> r{V(21.2,.2,0),V(0,0,1),0,10};bvh::v2::SmallStack<B::Index,64> st;size_t id=99; b.intersect<false,true>(r,b.get_root().index,st,[&](size_t lo,size_t hi){for(auto k=lo;k<hi;++k){auto j=b.prim_ids[k];bvh::v2::PrecomputedTri<double> t(ts[j]);if(auto h=t.intersect(r)){id=j;std::tie(r.tmax,std::ignore,std::ignore)=*h;}}return id!=99;});assert(id==7);assert(std::abs(r.tmax-expected)<1e-8);printf("BVH_QUERY id=%zu t=%.1f\n",id,r.tmax);};query(2);ts[7]=T(V(21,0,4),V(22,0,4),V(21,1,4));b.refit([&](N&n){auto box=bvh::v2::BBox<double,3>::make_empty();for(size_t k=n.index.first_id();k<n.index.first_id()+n.index.prim_count();++k)box.extend(ts[b.prim_ids[k]].get_bbox());n.set_bbox(box);});query(4);puts("BVH_BUILD_REFIT_BASELINE_OK");}
