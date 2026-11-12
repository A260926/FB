#include "sisl.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
int main(void) {
  double u[]={0,0,0,1,1,1},v[]={0,0,1,1};
  double c[]={0,0,.1875, .5,0,-.3125, 1,0,.1875,
              0,1,.1875, .5,1,-.3125, 1,1,.1875};
  double p[]={0,0,0},n[]={0,0,1},*uv=NULL;
  SISLSurf *s=newSurf(3,2,3,2,u,v,c,1,3,1);
  SISLIntcurve **curves=NULL; int np=0,nc=0,st=0;
  if(!s) return 2;
  s1851(s,p,n,3,1e-12,1e-7,&np,&uv,&nc,&curves,&st);
  printf("topology status=%d isolated=%d curves=%d\n",st,np,nc);
  if(st<0||np!=0||nc!=2) return 3;
  for(int i=0;i<nc;i++) {
    s1314(s,p,n,3,1e-12,1e-7,.1,curves[i],2,0,&st);
    if(st!=0||!curves[i]->pgeom||!curves[i]->ppar1) return 4;
    SISLCurve *g=curves[i]->pgeom; int left=0;
    double xyz[3],t=(g->et[g->ik-1]+g->et[g->in])*.5;
    s1227(g,0,t,&left,xyz,&st);
    printf("branch=%d midpoint=(%.8f,%.8f,%.8f)\n",i,xyz[0],xyz[1],xyz[2]);
    if(st<0||fabs(xyz[2])>1e-6||fmin(fabs(xyz[0]-.25),fabs(xyz[0]-.75))>1e-6) return 5;
  }
  freeIntcrvlist(curves,nc); free(uv); freeSurf(s);
  puts("SISL_BASELINE_OK branches=2 marched=2"); return 0;
}
