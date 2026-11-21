#include <algorithm>
#include <limits>
#include <iostream>
#include <cmath>
#include "rasterizer/renderer.h"
struct Vertex { Vec4 p; Vec3 c; };
struct Varying { Vec4 position; Vec3 color; VARYING(position,color); };
struct Uniforms {};
static void need(bool b){if(!b) std::exit(2);}
int main(){
 Renderer r(32,32);r.options().culling=false;
 Program<Vertex,Varying,Uniforms> p;
 unsigned calls=0;
 p.onVertex([](const Uniforms&,const Vertex& v,Varying& o){o.position=v.p;o.color=v.c;});
 p.onFragment([&](const Uniforms&,const Varying& v,Vec4& o){++calls;o=Vec4(v.color,1);});
 BufferIndexed<Vertex,unsigned> mesh;
 mesh.vertices={{{-.75f,-.75f,.5f,1},{1,0,0}},{{.75f,-.75f,.5f,1},{1,0,0}},{{0,.75f,.5f,1},{1,0,0}}};
 mesh.indices={0,1,2};r.draw(p,mesh);need(calls>0);unsigned first=calls;
 auto &fb=r.framebuffer();need(std::abs(fb.depth()(16,12)-.75f)<1e-6f);need(fb.color()(16,12).x==255);
 for(auto& v:mesh.vertices){v.p.z=-.5f;v.c={0,1,0};}r.draw(p,mesh);
 need(fb.color()(16,12).y==255);need(std::abs(fb.depth()(16,12)-.25f)<1e-6f);
 for(auto& v:mesh.vertices){v.p.z=.8f;v.c={0,0,1};}unsigned before=calls;r.draw(p,mesh);need(calls==before);
 Buffer<Vertex> plain;for(auto i:mesh.indices)plain.vertices.push_back(mesh.vertices[i]);
 Renderer other(32,32);other.options().culling=false;other.draw(p,plain);
 need(other.framebuffer().color()(16,12).z==255);
 std::cout<<"RASTER_BASELINE_OK indexed="<<first<<" near_depth=0.25 occluded_calls=0 nonindexed=blue\n";
}
