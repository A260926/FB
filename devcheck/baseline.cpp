#include <poly2tri/poly2tri.h>
#include <cassert>
#include <cmath>
#include <iostream>
#include <set>
int main(){
 p2t::Point a(0,0),b(10,0),c(10,8),d(0,8);
 p2t::Point e(3,2),f(3,5),g(6,5),h(6,2),s(1,3);
 std::vector<p2t::Point*> outer{&a,&b,&c,&d},hole{&e,&f,&g,&h};
 p2t::CDT cdt(outer); cdt.AddHole(hole);cdt.AddPoint(&s);cdt.Triangulate();
 const auto ts=cdt.GetTriangles();double area=0;std::set<p2t::Point*> used;
 assert(ts.size()==10);
 for(auto t:ts){
  auto p=t->GetPoint(0),q=t->GetPoint(1),r=t->GetPoint(2);
  double cross=(q->x-p->x)*(r->y-p->y)-(q->y-p->y)*(r->x-p->x);
  assert(cross>0);area+=cross*.5;
  for(int i=0;i<3;++i)used.insert(t->GetPoint(i));
 }
 assert(used.size()==9 && used.count(&s));assert(std::abs(area-71)<1e-10);
 for(int i=0;i<4;++i){
  int found=0;for(auto t:ts){int k=t->EdgeIndex(hole[i],hole[(i+1)%4]);if(k>=0){assert(t->constrained_edge[k]);++found;}}
  assert(found==1);
 }
 std::cout<<"POLY2TRI_BASELINE_OK triangles=10 area=71 vertices=9 hole-edges=4\n";
}
