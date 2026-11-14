#include "AABB.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main(){
 aabb::Tree tree(3,.5,16,true);
 std::vector<double> lo{.5,-.5,-.5},hi{1.5,.5,.5};
 tree.insertParticle(7,lo,hi);
 aabb::AABB sphereBox({-.6,-.6,-.6},{.6,.6,.6});
 auto before=tree.query(sphereBox);assert(before.size()==1&&before[0]==7);
 lo[0]=.8;hi[0]=1.8;
 bool reinsert=tree.updateParticle(7,lo,hi);assert(!reinsert);
 assert(std::fabs(tree.getAABB(7).lowerBound[0])<1e-12);
 auto after=tree.query(sphereBox);assert(after.size()==1&&after[0]==7);
 assert(lo[0]>.6); // The tight object no longer intersects the origin sphere.
 tree.validate();tree.removeParticle(7);
 assert(tree.query(sphereBox).empty());
 tree.insertParticle(42,lo,hi);tree.validate();
 auto reused=tree.query(sphereBox);assert(reused.size()==1&&reused[0]==42);
 aabb::Tree touch(3,0,16,true),strict(3,0,16,false);
 lo={.6,-.1,-.1};hi={.8,.1,.1};
 touch.insertParticle(1,lo,hi);strict.insertParticle(1,lo,hi);
 assert(touch.query(sphereBox).size()==1);assert(strict.query(sphereBox).empty());
 std::cout<<"AABB_BASELINE_OK fat_candidate=1 reinsert=0 tight_exit=1 reused_id=42 touch_modes=1\n";
}
