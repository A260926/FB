#include <renderer.h>
#include <cassert>
#include <iostream>
struct V { Vec4 p; };
struct F { Vec4 position; VARYING(position); };
struct U { Vec4 color; };
int main() {
 Program<V,F,U> p;
 p.onVertex([](const U&,const V& v,F& f){f.position=v.p;});
 p.onFragment([](const U& u,const F&,Vec4& c){c=u.color;});
 Buffer<V> b;b.vertices={{{-.8f,-.7f,0,1}},{{.65f,-.7f,0,1}},{{-.1f,.8f,0,1}}};
 Renderer r(8,8);r.options().culling=false;r.framebuffer().clear(Vec4(0,0,0,1));
 p.uniforms().color=Vec4(1,0,0,1);r.draw(p,b);
 auto first=r.framebuffer().color().data();int covered=0,partial=0;
 for(const auto& c:first){covered+=c.x==255;partial+=c.x!=0 && c.x!=255;}
 assert(covered>4 && covered<40 && partial==0);
 BufferIndexed<V,unsigned> idx;idx.vertices=b.vertices;idx.indices={0,1,2};
 r.framebuffer().clear(Vec4(0,0,0,1));r.draw(p,idx);
 assert(first==r.framebuffer().color().data());
 for(auto& v:b.vertices)v.p.z=.8f;
 p.uniforms().color=Vec4(0,0,1,1);r.draw(p,b);
 assert(first==r.framebuffer().color().data());
 std::cout<<"RASTER_BASELINE_OK covered="<<covered<<" partial="<<partial<<" indexed-and-depth=stable\n";
}
