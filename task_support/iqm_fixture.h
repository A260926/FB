#ifndef IQM_TASK_FIXTURE_H
#define IQM_TASK_FIXTURE_H
#include <array>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include "task_support/demo_api.h"

namespace fixture {
using V = std::array<double, 3>;
using A = std::array<std::array<double, 4>, 4>;
inline A identity() { A a{}; for(int i=0;i<4;++i) a[i][i]=1; return a; }
inline A mul(const A& a, const A& b) {
    A r{}; for(int i=0;i<4;++i) for(int j=0;j<4;++j)
        for(int k=0;k<4;++k) r[i][j]+=a[i][k]*b[k][j];
    return r;
}
inline A lerp(const A& a, const A& b, double t) {
    A r{};for(int i=0;i<4;++i)for(int j=0;j<4;++j)r[i][j]=(1-t)*a[i][j]+t*b[i][j];return r;
}
inline A trs(const std::array<float,10>& c) {
    double x=c[3],y=c[4],z=c[5],w=c[6],n=std::sqrt(x*x+y*y+z*z+w*w);
    x/=n;y/=n;z/=n;w/=n; A a=identity();
    double r[3][3]={{1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)},
                    {2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)},
                    {2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)}};
    for(int i=0;i<3;++i){for(int j=0;j<3;++j)a[i][j]=r[i][j]*c[7+j];a[i][3]=c[i];}
    return a;
}
inline A rigid_inverse(const A& a) {
    A r=identity();for(int i=0;i<3;++i){r[i][3]=0;for(int j=0;j<3;++j){r[i][j]=a[j][i];r[i][3]-=a[j][i]*a[j][3];}}return r;
}
inline V point(const A& a, const V& v) {
    V r{};for(int i=0;i<3;++i){r[i]=a[i][3];for(int j=0;j<3;++j)r[i]+=a[i][j]*v[j];}return r;
}
inline V cross(const V& a,const V& b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline V cofactor(const A& a,const V& v) {
    V r{};for(int i=0;i<3;++i)for(int j=0;j<3;++j){
        int r0=(i+1)%3,r1=(i+2)%3,c0=(j+1)%3,c1=(j+2)%3;
        r[i]+=(a[r0][c0]*a[r1][c1]-a[r0][c1]*a[r1][c0])*v[j];
    }return r;
}
struct Model {
    std::array<std::array<std::array<float,10>,2>,2> channels{}; // frame, joint
    float positions[12]={-.3f,.2f,-.4f, 1.2f,-.4f,.1f, .1f,1.1f,.2f, .2f,.3f,1.4f};
    float normals[12]={.2f,.6f,1.f, -.3f,1.f,.2f, 1.f,.4f,-.2f, .1f,-.6f,1.f};
    float tangents[16]={1.f,.1f,-.2f,1.f, .7f,-.2f,.4f,-1.f, -.2f,.8f,.4f,1.f, 1.f,.3f,.2f,-1.f};
    uchar indices[16]={0,0,0,0, 0,1,0,0, 1,0,0,0, 1,0,0,0};
    uchar weights[16]={255,0,0,0, 153,102,0,0, 191,64,0,0, 255,0,0,0};
    Model() {
        channels[0][0]={.2f,-.3f,.4f,0,0,float(std::sin(.2)),float(std::cos(.2)),1,1,1};
        channels[0][1]={1,.3f,.1f,0,float(std::sin(.3)),0,float(std::cos(.3)),1,1,1};
        channels[1][0]={.5f,-.1f,.7f,0,0,float(std::sin(.65)),float(std::cos(.65)),1,1,1};
        channels[1][1]={1.2f,.3f,-.1f,float(std::sin(.45)),0,0,float(std::cos(.45)),1.1f,.8f,1.3f};
    }
    std::array<A,2> palette(double f) const {
        int k=int(std::floor(f));double t=f-k;k%=2;
        A b0=trs(channels[0][0]),b1=mul(b0,trs(channels[0][1]));
        A p0=lerp(trs(channels[k][0]),trs(channels[1-k][0]),t);
        A p1=lerp(trs(channels[k][1]),trs(channels[1-k][1]),t);
        return {mul(p0,rigid_inverse(b0)),mul(mul(p0,p1),rigid_inverse(b1))};
    }
    static unsigned append(std::vector<uchar>& b,const void* src,size_t n) {
        while(b.size()%4)b.push_back(0);unsigned at=unsigned(b.size());b.resize(b.size()+n);
        if(n)std::memcpy(b.data()+at,src,n);return at;
    }
    void write(const char* name) const {
        if(!islittleendian())throw std::runtime_error("fixture writer requires a little-endian host");
        iqmheader h{};std::memcpy(h.magic,IQM_MAGIC,16);h.version=IQM_VERSION;
        std::vector<uchar> b(sizeof(h));
        const char text[]="fixture\0root\0child\0move\0";
        h.num_text=sizeof(text);h.ofs_text=append(b,text,sizeof(text));
        iqmmesh mesh{};mesh.num_vertexes=4;mesh.num_triangles=4;
        h.num_meshes=1;h.ofs_meshes=append(b,&mesh,sizeof(mesh));
        iqmtriangle tr[4]={{{0,2,1}},{{0,1,3}},{{1,2,3}},{{2,0,3}}};
        h.num_triangles=4;h.ofs_triangles=append(b,tr,sizeof(tr));
        iqmvertexarray va[6]{};h.num_vertexarrays=6;h.num_vertexes=4;
        h.ofs_vertexarrays=append(b,va,sizeof(va));
        const void* data[6]={positions,normals,tangents,nullptr,indices,weights};
        float uv[8]={0,0,1,0,0,1,1,1};data[3]=uv;
        unsigned type[6]={IQM_POSITION,IQM_NORMAL,IQM_TANGENT,IQM_TEXCOORD,IQM_BLENDINDEXES,IQM_BLENDWEIGHTS};
        unsigned size[6]={3,3,4,2,4,4};
        size_t bytes[6]={sizeof(positions),sizeof(normals),sizeof(tangents),sizeof(uv),sizeof(indices),sizeof(weights)};
        for(int i=0;i<6;++i){va[i].type=type[i];va[i].format=i<4?IQM_FLOAT:IQM_UBYTE;va[i].size=size[i];va[i].offset=append(b,data[i],bytes[i]);}
        std::memcpy(b.data()+h.ofs_vertexarrays,va,sizeof(va));
        iqmjoint j[2]{};iqmpose p[2]{};
        for(int k=0;k<2;++k){j[k].name=k?13:8;j[k].parent=k-1;
            std::memcpy(j[k].translate,channels[0][k].data(),3*sizeof(float));
            std::memcpy(j[k].rotate,channels[0][k].data()+3,4*sizeof(float));
            std::memcpy(j[k].scale,channels[0][k].data()+7,3*sizeof(float));
            p[k].parent=k-1;p[k].mask=0x3ff;
            for(int c=0;c<10;++c){float lo=std::min(channels[0][k][c],channels[1][k][c]);
                float hi=std::max(channels[0][k][c],channels[1][k][c]);p[k].channeloffset[c]=lo;p[k].channelscale[c]=(hi-lo)/65535.f;}}
        h.num_joints=2;h.ofs_joints=append(b,j,sizeof(j));h.num_poses=2;h.ofs_poses=append(b,p,sizeof(p));
        ushort keys[40]{};
        for(int f=0;f<2;++f)for(int k=0;k<2;++k)for(int c=0;c<10;++c)
            keys[f*20+k*10+c]=channels[f][k][c]>p[k].channeloffset[c]?65535:0;
        h.num_frames=2;h.num_framechannels=20;h.ofs_frames=append(b,keys,sizeof(keys));
        iqmanim anim{};anim.name=19;anim.num_frames=2;anim.framerate=12;anim.flags=IQM_LOOP;
        h.num_anims=1;h.ofs_anims=append(b,&anim,sizeof(anim));
        h.filesize=unsigned(b.size());std::memcpy(b.data(),&h,sizeof(h));
        std::ofstream out(name,std::ios::binary);out.write(reinterpret_cast<const char*>(b.data()),b.size());
        if(!out)throw std::runtime_error("cannot write IQM fixture");
    }
};
}
#endif
