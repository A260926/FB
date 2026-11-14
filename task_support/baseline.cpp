#include <cstddef>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>
#include "dualmc.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"BASELINE_FAIL line=%d %s\n",__LINE__,#x); return 1; } } while (0)
static bool near(float a,float b) { return std::fabs(a-b)<2e-6f; }
static bool same(const dualmc::Vertex &a,const dualmc::Vertex &b) {
    return near(a.x,b.x)&&near(a.y,b.y)&&near(a.z,b.z);
}
int main() {
    std::vector<float> field(125,0.0f);
    field[2+5*(2+5*2)]=1.0f;
    dualmc::DualMC<float> dmc;
    std::vector<dualmc::Vertex> v,s;
    std::vector<dualmc::Quad> q,t;
    for (int manifold=0;manifold<2;++manifold) {
        dmc.build(field.data(),5,5,5,0.5f,manifold,false,v,q);
        CHECK(v.size()==8 && q.size()==6);
        for (const auto &p:v) {
            CHECK(near(p.x,11.0f/6)||near(p.x,13.0f/6));
            CHECK(near(p.y,11.0f/6)||near(p.y,13.0f/6));
            CHECK(near(p.z,11.0f/6)||near(p.z,13.0f/6));
        }
        std::map<std::pair<int,int>,int> edges;
        for (const auto &f:q) {
            int ids[]={f.i0,f.i1,f.i2,f.i3};
            for (int k=0;k<4;++k) {
                CHECK(ids[k]>=0 && std::size_t(ids[k])<v.size());
                auto edge=std::minmax(ids[k],ids[(k+1)%4]);
                ++edges[{edge.first,edge.second}];
            }
            const auto &a=v[ids[0]], &b=v[ids[1]], &c=v[ids[2]];
            float nx=(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y);
            float ny=(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z);
            float nz=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
            CHECK(nx*(a.x-2)+ny*(a.y-2)+nz*(a.z-2)>0);
        }
        CHECK(edges.size()==12);
        for (const auto &edge:edges) CHECK(edge.second==2);
        dmc.build(field.data(),5,5,5,0.5f,manifold,true,s,t);
        CHECK(s.size()==24 && t.size()==6);
        for (std::size_t j=0;j<q.size();++j) {
            int ids[]={q[j].i0,q[j].i1,q[j].i2,q[j].i3};
            int soup[]={t[j].i0,t[j].i1,t[j].i2,t[j].i3};
            for(int k=0;k<4;++k) { CHECK(soup[k]==int(j*4+k)); CHECK(same(v[ids[k]],s[soup[k]])); }
        }
        dmc.build(field.data(),5,5,5,2.0f,manifold,false,v,q);
        CHECK(v.empty()&&q.empty());
    }
    std::puts("DUALMC_BASELINE_PASS shared=8 quads=6 soup=24 manifold=both outward=ok reuse=ok");
}
