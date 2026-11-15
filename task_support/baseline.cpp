#include "source/xatlas/xatlas.h"
#include "source/xatlas/xatlas_c.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <cstddef>
extern "C" int original_c_api_probe(void);
static_assert(sizeof(xatlas::MeshDecl)==sizeof(xatlasMeshDecl),"C MeshDecl layout");
static_assert(sizeof(xatlas::ChartOptions)==sizeof(xatlasChartOptions),"C ChartOptions layout");
static_assert(offsetof(xatlas::MeshDecl,vertexCount)==offsetof(xatlasMeshDecl,vertexCount),"vertexCount offset");
static bool check(xatlas::Atlas* a){
 if(a->meshCount!=1 || a->chartCount==0 || a->atlasCount==0 || a->width==0 || a->height==0)return false;
 const uint32_t source[12]={0,1,2,2,1,3,2,3,4,4,3,5};
 auto &m=a->meshes[0];if(m.indexCount!=12)return false;
 for(uint32_t i=0;i<12;i++){auto v=m.vertexArray[m.indexArray[i]];if(v.xref!=source[i] || v.chartIndex<0 || v.atlasIndex<0 || !std::isfinite(v.uv[0]) || !std::isfinite(v.uv[1]))return false;}
 return true;
}
int main(){
 xatlas::SetPrint(nullptr,false);
 if(original_c_api_probe()!=0)return 1;
 auto* a=xatlas::Create();
 std::vector<float> p={0,0,0, 2,0,0, 0,1,0, 2,1,0.5f, 0,3,0, 2,3,1};
 uint32_t ind[12]={0,1,2,2,1,3,2,3,4,4,3,5};uint32_t mat[4]={7,7,9,9};
 xatlas::MeshDecl d;d.vertexPositionData=p.data();d.vertexPositionStride=12;d.vertexCount=6;d.indexData=ind;d.indexCount=12;d.indexFormat=xatlas::IndexFormat::UInt32;d.faceMaterialData=mat;
 if(xatlas::AddMesh(a,d)!=xatlas::AddMeshError::Success){xatlas::Destroy(a);return 2;}
 std::vector<float>().swap(p);
 xatlas::ComputeCharts(a);
 xatlas::PackOptions po;po.texelsPerUnit=16;po.resolution=128;po.padding=1;po.bruteForce=true;
 xatlas::PackCharts(a,po);if(!check(a)){xatlas::Destroy(a);return 3;}
 auto charts=a->chartCount;po.padding=2;po.rotateCharts=false;xatlas::PackCharts(a,po);bool ok=check(a)&&charts==a->chartCount;
 for(uint32_t c=0;c<a->meshes[0].chartCount;c++){auto &chart=a->meshes[0].chartArray[c];for(uint32_t f=0;f<chart.faceCount;f++){uint32_t face=chart.faceArray[f];if(face>=4 || chart.faceCount==0 || mat[face]!=mat[chart.faceArray[0]])ok=false;}}
 if(ok)std::printf("XATLAS_PRODUCTION_OK faces=4 charts=%u source_release=ok xref=ok materials=ok repack=ok c_api=ok\n",a->chartCount);
 xatlas::Destroy(a);return ok?0:4;
}
