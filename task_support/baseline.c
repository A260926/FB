#include "sisl.h"
#include <math.h>
#include <stdio.h>
int main(void){
 double k[]={-2,-2,-2,-2,4,4,4,4},c[48];int i,j,s=0,a=0,b=0,a2=0,b2=0;double err=0;
 for(j=0;j<4;j++)for(i=0;i<4;i++){int n=3*(4*j+i);c[n]=i;c[n+1]=j;c[n+2]=0.12*i*j+0.03*i*i;}
 SISLSurf*p=newSurf(4,4,4,4,k,k,c,1,3,1),*r=0;if(!p)return 2;
 s1365(p,0.1,0.001,0,3,&r,&s);if(s<0||!r)return 3;
 for(j=0;j<3;j++)for(i=0;i<3;i++){
 double uv[]={-2+3.0*i,-2+3.0*j},x[9],y[9],n[3],m[3];
 s1421(p,1,uv,&a,&b,x,n,&s);if(s<0)return 4;s1421(r,1,uv,&a2,&b2,y,m,&s);if(s<0)return 5;
 double len=sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);if(!(len>0))return 6;
 double e=0;for(int z=0;z<3;z++){double d=y[z]-x[z]-0.1*n[z]/len;e+=d*d;if(!isfinite(y[z]))return 7;}if(sqrt(e)>err)err=sqrt(e);
 }
 printf("SISL_OFFSET_BASELINE samples=9 output=%dx%d max_offset_error=%.17g\n",r->in1,r->in2,err);
 freeSurf(p);freeSurf(r);return err<0.01?0:8;
}
