#include <cstddef>
#include <cmath>
#include <iostream>
#include <vector>
#include "dualmc.h"
int main() {
    const int n=7;
    std::vector<float> data(n*n*n);
    for(int z=0;z<n;++z) for(int y=0;y<n;++y) for(int x=0;x<n;++x)
        data[x+n*(y+n*z)]=float(x)-2.25f;
    dualmc::DualMC<float> d;
    std::vector<dualmc::Vertex> v;
    std::vector<dualmc::Quad> q;
    d.build(data.data(),n,n,n,0.f,true,false,v,q);
    if(v.size()!=25 || q.size()!=16) return 1;
    for(auto p:v) if(std::fabs(p.x-2.25f)>1e-6f) return 2;
    for(auto a:q) {
        int ids[]={a.i0,a.i1,a.i2,a.i3};
        for(int i:ids) if(i<0 || size_t(i)>=v.size()) return 3;
        auto a0=v[a.i0], b=v[a.i1], c=v[a.i2];
        float cross=(b.y-a0.y)*(c.z-a0.z)-(b.z-a0.z)*(c.y-a0.y);
        if(!(cross<0.f)) return 4;
    }
    std::cout<<"PLANE vertices="<<v.size()<<" quads="<<q.size()<<" x=2.25 winding=-x\n";
    d.build(data.data(),n,n,n,0.f,true,true,v,q);
    if(v.size()!=64 || q.size()!=16) return 5;
    std::cout<<"SOUP vertices="<<v.size()<<" quads="<<q.size()<<"\n";
    for(int z=0;z<n;++z) for(int y=0;y<n;++y) for(int x=0;x<n;++x)
        data[x+n*(y+n*z)]=float((x-3)*(x-3)+(y-3)*(y-3)+(z-3)*(z-3))-2.5f;
    d.build(data.data(),n,n,n,0.f,true,false,v,q);
    if(v.empty()||q.empty()) return 6;
    for(auto p:v) if(!std::isfinite(p.x+p.y+p.z)) return 7;
    std::cout<<"CLOSED vertices="<<v.size()<<" quads="<<q.size()<<" finite=1\nBASELINE_COMPLETE\n";
}
