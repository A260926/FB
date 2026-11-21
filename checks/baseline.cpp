#include "source/xatlas/xatlas.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
static void check(bool b){if(!b){std::fprintf(stderr,"BASELINE_FAIL\n");std::exit(2);}}
int main(){
 float p[]={0,0,0, 1,0,0, 0,1,0, 1,1,0, 0,2,1, 1,2,1};
 unsigned int ix[]={0,1,3,0,3,2,2,3,5,2,5,4}, mat[]={1,1,2,2};
 xatlas::MeshDecl d;d.vertexPositionData=p;d.vertexCount=6;d.vertexPositionStride=12;d.indexData=ix;d.indexCount=12;d.indexFormat=xatlas::IndexFormat::UInt32;d.faceMaterialData=mat;
 auto a=xatlas::Create();check(xatlas::AddMesh(a,d)==xatlas::AddMeshError::Success);
 xatlas::ComputeCharts(a);xatlas::PackOptions opt;opt.resolution=64;opt.padding=2;xatlas::PackCharts(a,opt);
 check(a->meshCount==1&&a->chartCount>=2&&a->width>0&&a->height>0);
 auto& m=a->meshes[0];check(m.indexCount==12&&m.vertexCount>=6);
 double total=0;for(unsigned f=0;f<4;++f){auto v0=m.vertexArray[m.indexArray[3*f]],v1=m.vertexArray[m.indexArray[3*f+1]],v2=m.vertexArray[m.indexArray[3*f+2]];
 for(auto v:{v0,v1,v2})check(v.xref<6&&std::isfinite(v.uv[0])&&std::isfinite(v.uv[1]));
 double area=(v1.uv[0]-v0.uv[0])*(v2.uv[1]-v0.uv[1])-(v1.uv[1]-v0.uv[1])*(v2.uv[0]-v0.uv[0]);check(std::abs(area)>1e-6);total+=std::abs(area)/2;}
 std::printf("BASELINE charts=%u faces=4 uv_area=%.6f xref_ok=1\n",a->chartCount,total);
 xatlas::PackCharts(a,opt);check(a->meshCount==1&&a->meshes[0].indexCount==12);std::puts("BASELINE_REPACK_OK");xatlas::Destroy(a);
}
