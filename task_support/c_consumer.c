#include "source/xatlas/xatlas_c.h"
#include <stddef.h>
int main(void) {
    float p[]={0,0,0,1,0,0,0,1,0};uint32_t idx[]={0,1,2};uint8_t flags[]={0,0,0};
    xatlasMeshDecl d;xatlasMeshDeclInit(&d);d.vertexPositionData=p;d.vertexCount=3;d.vertexPositionStride=12;d.indexData=idx;d.indexCount=3;d.indexFormat=XATLAS_INDEX_FORMAT_UINT32;
    xatlasAtlas*a=xatlasCreate();xatlasChartOptions o;xatlasChartOptionsInit(&o);xatlasSetPrint(NULL,false);
    if(xatlasAddMesh(a,&d,0)!=XATLAS_ADD_MESH_ERROR_SUCCESS)return 1;
    if(!xatlasSetMeshChartConstraints(a,0,flags,3)||!xatlasComputeChartsConstrained(a,&o))return 2;
    xatlasPackCharts(a,NULL);
    if(a->meshCount!=1||a->chartCount!=1||a->meshes[0].indexCount!=3)return 3;
    for(unsigned i=0;i<3;++i)if(a->meshes[0].vertexArray[a->meshes[0].indexArray[i]].xref!=i)return 4;
    xatlasDestroy(a);return 0;
}
