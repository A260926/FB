#include "source/xatlas/xatlas.h"
#include <cmath>
#include <cstdio>
int main() {
    xatlas::SetPrint(nullptr,false);
    float p[]={0,0,0, 1,0,0, 1,1,0, 0,1,0};uint32_t indices[]={0,1,2,0,2,3};
    xatlas::MeshDecl d;d.vertexPositionData=p;d.vertexCount=4;d.vertexPositionStride=12;d.indexData=indices;d.indexCount=6;d.indexFormat=xatlas::IndexFormat::UInt32;
    xatlas::Atlas*a=xatlas::Create();if(xatlas::AddMesh(a,d)!=xatlas::AddMeshError::Success)return 1;
    xatlas::ComputeCharts(a);xatlas::PackOptions o;o.resolution=64;o.texelsPerUnit=12;o.padding=1;
    for(int pass=0;pass<2;++pass) {
        o.rotateCharts=pass==0;xatlas::PackCharts(a,o);
        if(a->meshCount!=1||a->chartCount!=1||a->meshes[0].indexCount!=6)return 2;
        auto&m=a->meshes[0];double area=0;
        for(unsigned i=0;i<6;i+=3){const auto&v0=m.vertexArray[m.indexArray[i]];const auto&v1=m.vertexArray[m.indexArray[i+1]];const auto&v2=m.vertexArray[m.indexArray[i+2]];
            if(v0.xref!=indices[i]||v1.xref!=indices[i+1]||v2.xref!=indices[i+2])return 3;
            double cross=(v1.uv[0]-v0.uv[0])*(v2.uv[1]-v0.uv[1])-(v1.uv[1]-v0.uv[1])*(v2.uv[0]-v0.uv[0]);
            if(!std::isfinite(cross)||std::abs(cross)<1)return 4;area+=std::abs(cross)*0.5;
        }
        std::printf("XATLAS_BASELINE pass=%d charts=%u indices=%u uv_area=%.3f xref=ok\n",pass,a->chartCount,m.indexCount,area);
    }
    xatlas::Destroy(a);
}
