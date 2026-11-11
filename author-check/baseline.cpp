#include "xatlas.h"
#include <cassert>
#include <cstdio>
int main() {
 float p[]={0,0,0, 2,0,0, 2,1,0, 0,1,0};
 uint32_t idx[]={0,1,2, 0,2,3}, mat[]={7,19};
 xatlas::MeshDecl d; d.vertexPositionData=p; d.vertexCount=4;
 d.vertexPositionStride=12; d.indexData=idx; d.indexCount=6;
 d.indexFormat=xatlas::IndexFormat::UInt32; d.faceMaterialData=mat;
 auto a=xatlas::Create();
 assert(xatlas::AddMesh(a,d)==xatlas::AddMeshError::Success);
 xatlas::PackOptions o; o.resolution=64; o.texelsPerUnit=8;
 o.padding=2; o.bruteForce=true; o.createImage=true;
 xatlas::Generate(a,xatlas::ChartOptions(),o);
 assert(a->meshCount==1 && a->chartCount==2 && a->meshes[0].indexCount==6);
 auto &m=a->meshes[0];
 for(unsigned k=0;k<6;k++) assert(m.vertexArray[m.indexArray[k]].xref==idx[k]);
 for(unsigned c=0;c<m.chartCount;c++) {
  assert(m.chartArray[c].faceCount==1);
  assert(m.chartArray[c].faceArray[0]<2);
 }
 printf("BASELINE_XATLAS charts=%u faces=2 xref=preserved image=present\n",a->chartCount);
 assert(a->image!=nullptr); xatlas::Destroy(a);
}
