#include <cmath>
#include <iostream>
#include "renderer.h"
#include "sampler.h"
struct Vertex { Vec4 p; Vec2 uv; };
struct Varying { Vec4 position; Vec2 uv; VARYING(position, uv); };
struct Uniforms { Sampler<RGBA8> tex; };
int main() {
    Texture<RGBA8> image(2,2,RGBA8(180,60,20,255));
    Program<Vertex,Varying,Uniforms> program;
    unsigned fragments=0;
    program.onVertex([](const Uniforms&,const Vertex& v,Varying& o){o.position=v.p; o.uv=v.uv;});
    program.onFragment([&](const Uniforms& u,const Varying& v,Vec4& o){++fragments; o=texture(u.tex,v.uv)/255.f;});
    program.uniforms().tex=image;
    program.uniforms().tex.filter=eFilter::NEAREST;
    BufferIndexed<Vertex,unsigned> mesh;
    mesh.vertices={{{-.8f,-.8f,0,1},{0,0}},{{.8f,-.8f,0,1},{1,0}},{{.8f,.8f,0,1},{1,1}},{{-.8f,.8f,0,1},{0,1}}};
    mesh.indices={0,1,2,0,2,3};
    Renderer r(24,24);
    r.options().culling=false;
    r.draw(program,mesh);
    auto a=r.framebuffer().color()(12,10);
    if(a.x!=180 || a.y!=60 || a.z!=20 || fragments<100) return 1;
    unsigned first=fragments;
    for(auto& v:mesh.vertices)v.p.z=.8f;
    r.draw(program,mesh);
    if(fragments!=first || std::abs(r.framebuffer().depth()(12,10)-.5f)>1e-6f) return 2;
    r.framebuffer().clear(Vec4(0));
    Buffer<Vertex> plain;
    for(auto i:mesh.indices)plain.vertices.push_back(mesh.vertices[i]);
    r.draw(program,plain);
    auto b=r.framebuffer().color()(12,10);
    if(b.x!=180 || fragments<=first) return 3;
    std::cout<<"DENSE_DRAW_OK indexed_fragments="<<first<<" plain_fragments="<<fragments-first<<" color="<<(int)b.x<<","<<(int)b.y<<","<<(int)b.z<<" depth="<<r.framebuffer().depth()(12,10)<<"\n";
}
