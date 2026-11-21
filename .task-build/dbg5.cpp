#include <OpenVolumeMesh/Unstable/Topology/PlanePartition.hh>
#include <iostream>
#include <map>
using namespace OpenVolumeMesh;
using Vec = Geometry::Vec3d;
static const int kHexFaces[6][4] = {
    {3,2,1,0},{7,6,5,4},{1,2,6,7},{4,5,3,0},{1,7,4,0},{2,3,5,6}};
static HFH inward(GeometricPolyhedralMeshV3d& m, FH fh, const Vec& c){
    auto vs=m.get_halfface_vertices(fh.h0()); Vec fc(0,0,0);
    for(auto v:vs) fc+=m.vertex(v); fc*=1.0/vs.size();
    Vec w=(m.vertex(vs[1])-m.vertex(vs[0])).cross(m.vertex(vs[2])-m.vertex(vs[1]));
    return w.dot(c-fc)>0?fh.h0():fh.h1();
}
static CH addhex(GeometricPolyhedralMeshV3d& m, const std::vector<VH>& v){
    Vec c(0,0,0); for(auto vh:v) c+=m.vertex(vh); c*=0.125;
    std::vector<HFH> hfs;
    for(auto& f:kHexFaces){
        std::vector<VH> loop{v[f[0]],v[f[1]],v[f[2]],v[f[3]]};
        HFH found=m.find_halfface(loop);
        FH fh=found.is_valid()?found.face_handle():m.add_face(loop);
        hfs.push_back(inward(m,fh,c));
    }
    return m.add_cell(hfs);
}
int main(){
    GeometricPolyhedralMeshV3d m;
    m.enable_bottom_up_incidences(true);
    m.enable_deferred_deletion(true);
    for(int x=0;x<=2;++x)for(int y=0;y<=1;++y)for(int z=0;z<=1;++z)
        m.add_vertex(Vec(x,y,z));
    auto vidx=[](int x,int y,int z){return VH(x*4+y*2+z);};
    std::vector<VH> L(8),R(8);
    for(int i=0;i<8;++i){
        int x=(i==1||i==2||i==6||i==7)?1:0;
        int y=(i==2||i==3||i==5||i==6)?1:0;
        int z=(i>=4)?1:0;
        L[i]=vidx(x,y,z); R[i]=vidx(x+1,y,z);
    }
    CH cL=addhex(m,L), cR=addhex(m,R);
    auto uv=m.request_halfface_property<std::vector<double>>("hf:uv",{});
    for(HFH hfh:m.halffaces()){
        size_t k=m.face(hfh.face_handle()).halfedges().size();
        std::vector<double> v; for(size_t i=0;i<k;++i){v.push_back(1000.0+hfh.idx()); v.push_back((double)i+0.25*hfh.idx());}
        uv[hfh]=v;
    }
    FH top=FH(-1);
    for(FH fh:m.faces()){
        auto vs=m.get_halfface_vertices(fh.h0()); bool rt=vs.size()==4;
        for(auto v:vs){auto p=m.vertex(v); if(std::abs(p[2]-1)>1e-12||p[0]<1-1e-12) rt=false;}
        if(rt){top=fh;break;}
    }
    std::cout<<"top="<<top<<" h0="<<top.h0().idx()<<" h1="<<top.h1().idx()<<"\n";
    for(unsigned char s=0;s<2;++s){
        HFH hfh=top.halfface_handle(s);
        for(auto vh:m.get_halfface_vertices(hfh))
            std::cout<<"snap h"<<(int)s<<" v"<<vh.idx()<<"\n";
    }
    partition_cells(m,{cL},Vec(0,1,0),-0.5);
    std::cout<<"after top size="<<m.face(top).halfedges().size()<<"\n";
    for(unsigned char s=0;s<2;++s){
        HFH hfh=top.halfface_handle(s);
        auto hes=m.halfface(hfh).halfedges();
        for(size_t i=0;i<hes.size();++i){
            VH vh=m.from_vertex_handle(hes[i]);
            std::cout<<"after h"<<(int)s<<" v"<<vh.idx()<<" p=("<<m.vertex(vh)[0]<<","<<m.vertex(vh)[1]<<") uv=("<<uv[hfh][2*i]<<","<<uv[hfh][2*i+1]<<")\n";
        }
    }
}
