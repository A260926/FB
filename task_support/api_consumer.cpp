#include "source/xatlas/xatlas.h"
int main() {
    xatlas::SetPrint(nullptr,false);auto*a=xatlas::Create();
    float p[]={0,0,0,1,0,0,0,1,0};unsigned idx[]={0,1,2};
    xatlas::MeshDecl d;d.vertexPositionData=p;d.vertexCount=3;d.vertexPositionStride=12;d.indexData=idx;d.indexCount=3;d.indexFormat=xatlas::IndexFormat::UInt32;
    if(xatlas::AddMesh(a,d)!=xatlas::AddMeshError::Success)return 1;
    uint8_t flags[3]={0,0,0};
    if(!xatlas::SetMeshChartConstraints(a,0,flags,3)||!xatlas::ComputeChartsConstrained(a,xatlas::ChartOptions()))return 2;
    xatlas::PackCharts(a);
    if(a->chartCount!=1||a->meshCount!=1||a->meshes[0].indexCount!=3)return 3;
    for(int i=0;i<3;++i)if(a->meshes[0].vertexArray[a->meshes[0].indexArray[i]].xref!=unsigned(i))return 4;
    xatlas::Destroy(a);
}
