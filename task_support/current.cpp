#include <cstdio>
#include <cmath>
#include <cstring>
#include "task_support/iqm_fixture.h"

static bool close(double a,double b) { return std::isfinite(a)&&std::fabs(a-b)<2e-5; }
static bool vec_close(const float* got,const fixture::V& want,const char* attr,int v,double frame) {
    for(int c=0;c<3;++c)if(!close(got[c],want[c])){
        std::fprintf(stderr,"BASELINE_DIFFERENCE attr=%s vertex=%d component=%d frame=%.6f got=%.9g expected=%.9g\n",attr,v,c,frame,double(got[c]),want[c]);return false;
    }return true;
}
int main() {
    fixture::Model model;model.write(".task-build/current.iqm");
    if(!loadiqm(".task-build/current.iqm")||numverts!=4||numjoints!=2||numframes!=2||numanims!=1)return 10;
    if(std::fabs(inversebaseframe[1].a.w)<.1f)return 11;
    const double times[]={0,.375,1,1.625,2.375};int values=0;
    for(double time:times){
        animateiqm(float(time));auto palette=model.palette(time);
        for(int j=0;j<2;++j){const Vec4 rows[]={outframe[j].a,outframe[j].b,outframe[j].c};
            for(int r=0;r<3;++r){const double got[]={rows[r].x,rows[r].y,rows[r].z,rows[r].w};
                for(int c=0;c<4;++c){if(!close(got[c],palette[j][r][c])){std::fprintf(stderr,"PALETTE_DIFFERENCE joint=%d row=%d col=%d\n",j,r,c);return 12;}++values;}}}
        for(int v=0;v<4;++v){fixture::A blended{};
            for(int k=0;k<4;++k){double weight=model.weights[4*v+k]/255.;int j=model.indices[4*v+k];
                for(int r=0;r<4;++r)for(int c=0;c<4;++c)blended[r][c]+=weight*palette[j][r][c];}
            fixture::V x={model.positions[3*v],model.positions[3*v+1],model.positions[3*v+2]};
            fixture::V n={model.normals[3*v],model.normals[3*v+1],model.normals[3*v+2]};
            fixture::V t={model.tangents[4*v],model.tangents[4*v+1],model.tangents[4*v+2]};
            auto en=fixture::cofactor(blended,n),et=fixture::cofactor(blended,t),eb=fixture::cross(en,et);
            for(auto& q:eb)q*=model.tangents[4*v+3];
            if(!vec_close(outposition+3*v,fixture::point(blended,x),"position",v,time)
                ||!vec_close(outnormal+3*v,en,"normal",v,time)
                ||!vec_close(outtangent+3*v,et,"tangent",v,time)
                ||!vec_close(outbitangent+3*v,eb,"bitangent",v,time))return 13;
            values+=12;
        }
    }
    if(std::memcmp(inposition,model.positions,sizeof(model.positions))
       ||std::memcmp(innormal,model.normals,sizeof(model.normals))
       ||std::memcmp(intangent,model.tangents,sizeof(model.tangents)))return 14;
    std::printf("IQM_CURRENT_OK frames=5 scalar_checks=%d attributes=position,normal,tangent,bitangent\n",values);
    cleanupiqm();delete[] meshdata;
    return 0;
}
