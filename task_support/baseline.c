#include "sisl.h"
#include <math.h>
#include <stdio.h>
int main(void){double k[]={0,0,1,1},cp[]={0,0,0, 1,0,0, 0,1,0, 1,1,1};
 double ck[]={2,2,2,5,5,5},uv[]={0,0, .5,0, 1,1};
 SISLSurf*s=newSurf(2,2,2,2,k,k,cp,1,3,1);SISLCurve*c=newCurve(3,3,ck,uv,1,2,1),*b=0;int st=0,l=0,a=0,d=0;
 if(!s||!c)return 1;s1730(c,&b,&st);if(st<0||!b)return 2;
 for(int i=0;i<=16;i++){double t=2+3*i/16.0,v[6],p[3],n[3];s1221(b,0,t,&l,v,&st);if(st<0)return 3;s1421(s,0,v,&a,&d,p,n,&st);if(st<0||fabs(p[0]-i/16.0)>1e-12||fabs(p[2]-pow(i/16.0,3))>1e-12)return 4;}
 freeCurve(b);freeCurve(c);freeSurf(s);puts("SISL_BASELINE samples=17 composition=pointwise bezier=ok");return 0;}
