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
static double vol(GeometricPolyhedralMeshV3d& m, CH ch){
    double v=0;
    for(HFH hfh:m.cell_halffaces(ch)){
        auto vs=m.get_halfface_vertices(hfh);
        for(size_t i=1;i+1<vs.size();++i){
            Vec a=m.vertex(vs[0]),b=m.vertex(vs[i]),c=m.vertex(vs[i+1]);
            v-=a.dot(b.cross(c))/6.0;
        }
    }
    return v;
}
static CH make_cube(GeometricPolyhedralMeshV3d& m, const Vec& o){
    std::vector<VH> v(8);
    for(int i=0;i<8;++i){
        double dx=(i==1||i==2||i==6||i==7)?1.0:0.0;
        double dy=(i==2||i==3||i==5||i==6)?1.0:0.0;
        double dz=(i>=4)?1.0:0.0;
        v[i]=m.add_vertex(o+Vec(dx,dy,dz));
    }
    return addhex(m,v);
}
int main(){
    GeometricPolyhedralMeshV3d m;
    m.enable_bottom_up_incidences(true);
    m.enable_deferred_deletion(true);
    CH nearc=make_cube(m,Vec(0,0,0));
    CH farc=make_cube(m,Vec(10,0,0));
    double before=0; for(auto c:m.cells()) before+=vol(m,c);
    std::cout<<"before total="<<before<<"\n";
    auto first=partition_cells(m,{nearc},Vec(1,0,0),-0.5);
    double t=0; for(auto c:m.cells()) t+=vol(m,c);
    std::cout<<"after 1st total="<<t<<"\n";
    for(auto c:m.cells()) std::cout<<" cell "<<c<<" vol="<<vol(m,c)<<" hfs="<<m.valence(c)<<"\n";
    CH pos=first.front().positive;
    auto second=partition_cells(m,{pos,farc},Vec(0,1,0),-0.5);
    std::cout<<"2nd records="<<second.size()<<"\n";
    t=0; for(auto c:m.cells()){ double vv=vol(m,c); std::cout<<" cell "<<c<<" vol="<<vv<<" hfs="<<m.valence(c)<<"\n"; t+=vv; }
    std::cout<<"after 2nd total="<<t<<"\n";
    // shell check
    for(auto ch:m.cells()){
        std::map<HEH,int> cnt;
        for(HFH hfh:m.cell_halffaces(ch)) for(HEH he:m.halfface(hfh).halfedges()) ++cnt[he];
        bool closed=true;
        for(auto& e:cnt){ if(e.second!=1||!cnt.count(e.first.opposite_handle())||cnt[e.first.opposite_handle()]!=1) closed=false; }
        if(!closed) std::cout<<"cell "<<ch<<" NOT CLOSED\n";
    }
}
