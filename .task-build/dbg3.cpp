#include <OpenVolumeMesh/Unstable/Topology/PlanePartition.hh>
#include <iostream>
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
    partition_cells(m,{cL},Vec(0,1,0),-0.5);
    for(FH fh:m.faces()){
        auto vs=m.get_halfface_vertices(fh.h0());
        bool rt=vs.size()==4; Vec p0;
        for(auto v:vs){auto p=m.vertex(v); if(std::abs(p[2]-1)>1e-12||p[0]<1-1e-12) rt=false;}
        if(rt){ std::cout<<"candidate "<<fh<<" size="<<m.face(fh).halfedges().size()<<"\n"; }
    }
    // list all faces of R top region
    for(HFH hfh:m.cell_halffaces(cR)){
        auto vs=m.get_halfface_vertices(hfh);
        bool z1=true; for(auto v:vs) if(std::abs(m.vertex(v)[2]-1)>1e-12) z1=false;
        if(z1) std::cout<<"R top hfh "<<hfh<<" face "<<hfh.face_handle()<<" size="<<m.face(hfh.face_handle()).halfedges().size()<<"\n";
    }
}
