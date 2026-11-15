#include "scene-under-test.h"
#include <iostream>
int main(){
 Scene sc;sc.ms.push_back({T::D,V(.7,.5,.3)});
 sc.os.push_back({T::D,&sc.ms[0]});
 for(int i=0;i<12;++i){int n=int(sc.G.ps.size());double x=i*3.;
  sc.G.ps.insert(sc.G.ps.end(),{V(x,0,3),V(x+1,0,3),V(x,1,3)});
  sc.os[0].fs.insert(sc.os[0].fs.end(),{Ind{n},Ind{n+1},Ind{n+2}});
 }
 sc.build();
 auto h=sc.isect({V(.2,.2,0),V(0,0,1)});
 if(!h||h->o!=&sc.os[0]||std::abs(h->sp.p.z-3)>1e-12)return 2;
 if(sc.isect({V(.2,.2,0),V(0,0,1)},Eps,2.9))return 3;
 auto bs=h->o->ev(T::D,h->sp,V(0,0,1),V(0,0,1));
 if(std::abs(bs.x-.7/Pi)>1e-12)return 4;
 std::cout<<"BASELINE minpt triangles="<<sc.trs.size()<<" hit_z="<<h->sp.p.z<<" material="<<bs.x<<" finite_shadow=clear\n";
}
