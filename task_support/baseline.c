#include "tesselator.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>
int main(void){
 const float outer[]={0,0,3,0,3,3,0,3},hole[]={1,1,2,1,2,2,1,2},normal[]={0,0,1};
 TESStesselator*t=tessNewTess(NULL);assert(t);
 tessAddContour(t,2,outer,8,4);tessSetOption(t,TESS_REVERSE_CONTOURS,1);tessAddContour(t,2,hole,8,4);
 assert(tessTesselate(t,TESS_WINDING_POSITIVE,TESS_POLYGONS,3,2,normal));
 assert(tessGetElementCount(t)==8);const int*e=tessGetElements(t);const float*v=tessGetVertices(t);double area=0;
 for(int i=0;i<8;++i){const float*a=v+2*e[3*i],*b=v+2*e[3*i+1],*c=v+2*e[3*i+2];area+=fabs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))/2;}
 assert(fabs(area-8)<1e-6);printf("LIBTESS_HOLE_BASELINE_OK triangles=8 area=%.0f\n",area);tessDeleteTess(t);return 0;
}
