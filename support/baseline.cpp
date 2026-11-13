#include <poly2tri/poly2tri.h>
#include <cmath>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>
static void require(bool v) { if(!v) throw std::runtime_error("baseline geometry failed"); }
int main() {
  // A surface patch in parameter coordinates, with a hole and interior handles.
  std::vector<p2t::Point> outer{{0,0},{12,0},{12,10},{0,10}};
  std::vector<p2t::Point> hole{{5,4},{5,6},{7,6},{7,4}};
  std::vector<p2t::Point> handles{{2,2},{4,2},{8,2},{10,2},{2,8},{10,8}};
  std::vector<p2t::Point*> op,hp;
  std::set<p2t::Point*> expected,seen;
  for(auto& p:outer) { op.push_back(&p); expected.insert(&p); }
  for(auto& p:hole) { hp.push_back(&p); expected.insert(&p); }
  for(auto& p:handles) expected.insert(&p);
  p2t::CDT cdt(op); cdt.AddHole(hp);
  for(auto& p:handles) cdt.AddPoint(&p);
  cdt.Triangulate();
  double area=0; unsigned boundary=0;
  auto ts=cdt.GetTriangles(); require(ts.size()==20);
  for(auto* t:ts) {
    auto *a=t->GetPoint(0),*b=t->GetPoint(1),*c=t->GetPoint(2);
    require(expected.count(a)&&expected.count(b)&&expected.count(c));
    seen.insert(a); seen.insert(b); seen.insert(c);
    area+=std::abs((b->x-a->x)*(c->y-a->y)-(b->y-a->y)*(c->x-a->x))/2;
    for(int i=0;i<3;i++) if(t->constrained_edge[i]) ++boundary;
  }
  require(std::abs(area-116)<1e-9); require(boundary==8); require(seen==expected);
  require(!cdt.GetMap().empty());
  std::cout<<"POLY2TRI_STATIC_SURFACE_OK triangles="<<ts.size()<<" area="<<area<<" boundary="<<boundary<<"\n";
  std::cout<<"EXISTING_SWEEP_HAS_NO_POSTBUILD_CONSTRAINT_EDIT\n";
}
