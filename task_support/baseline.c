#include "mikktspace.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef struct {int count;float p[4][3],uv[4][2];} Face;
static Face f[]={
 {3,{{0,0,0},{2,0,0},{0,1,0}},{{0,0},{1,0},{.5f,1}}},
 {3,{{4,0,0},{6,0,0},{4,1,0}},{{0,0},{-1,0},{-.5f,1}}},
 {4,{{10,0,0},{11,0,0},{11,1,0},{10,1,0}},{{0,0},{1,0},{1,1},{0,1}}},
 {3,{{0,0,0},{2,0,0},{0,0,0}},{{0,0},{1,0},{0,0}}}};
static int calls,mirrors,bad;
static int faces(const SMikkTSpaceContext*c){(void)c;return 4;}
static int corners(const SMikkTSpaceContext*c,int a){(void)c;return f[a].count;}
static void pos(const SMikkTSpaceContext*c,float*v,int a,int b){(void)c;memcpy(v,f[a].p[b],3*sizeof(float));}
static void norm(const SMikkTSpaceContext*c,float*v,int a,int b){(void)c;(void)a;(void)b;v[0]=v[1]=0;v[2]=1;}
static void uv(const SMikkTSpaceContext*c,float*v,int a,int b){(void)c;memcpy(v,f[a].uv[b],2*sizeof(float));}
static void result(const SMikkTSpaceContext*c,const float*t,const float*b,float s,float u,tbool o,int a,int v){
 (void)c;(void)v;calls++;if(!o)mirrors++;
 for(int k=0;k<3;k++)if(!isfinite(t[k])||!isfinite(b[k]))bad=1;
 if(a==0&&(fabs(t[0]-1)>1e-6||fabs(b[0]+sqrt(.5))>1e-6||fabs(b[1]-sqrt(.5))>1e-6||fabs(s-2)>1e-6||fabs(u-sqrt(2))>1e-6||!o))bad=2;
 if(a==1&&(t[0]>-.999||o))bad=3;
}
int main(void){SMikkTSpaceInterface i={faces,corners,pos,norm,uv,0,result};SMikkTSpaceContext c={&i,0};if(!genTangSpace(&c,180)||calls!=13||mirrors!=3||bad)return 1;printf("MIKK_BASELINE faces=4 corners=%d mirrored=%d full_bitangent=skew quad=ok degenerate=finite\n",calls,mirrors);return 0;}
