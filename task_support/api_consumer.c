#include "par_shapes.h"
#include <math.h>
#include <stdio.h>
int main(void) {
 par_shapes_mesh *p=par_shapes_create_plane(2,2);
 if(!p || !par_shapes_compute_tangents(p) || !p->tangents) return 21;
 for(int i=0;i<p->npoints;i++) {
  const float *t=p->tangents+4*i,*n=p->normals+3*i;
  if(!isfinite(t[0])||!isfinite(t[1])||!isfinite(t[2])||
     fabs(t[0]-1)>1e-5||fabs(t[1])>1e-5||fabs(t[2])>1e-5||t[3]!=1 ||
     fabs(n[0]*t[0]+n[1]*t[1]+n[2]*t[2])>1e-5) return 22;
 }
 printf("TANGENT_API points=%d T=1,0,0 w=1\n",p->npoints);
 par_shapes_free_mesh(p); return 0;
}
