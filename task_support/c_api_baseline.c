#include "source/xatlas/xatlas_c.h"
int original_c_api_probe(void){
 xatlasMeshDecl d; xatlasMeshDeclInit(&d);
 xatlasChartOptions c; xatlasChartOptionsInit(&c);
 xatlasAtlas* a=xatlasCreate();if(!a)return 1;
 int ok=d.vertexPositionData==0 && d.vertexCount==0 && c.paramFunc==0;
 xatlasDestroy(a);return ok?0:2;
}
