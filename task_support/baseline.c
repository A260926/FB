#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "mmg/mmgs/libmmgs.h"
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"baseline failure: %s:%d %s\n",__FILE__,__LINE__,#x); return 1; } } while (0)
int main(void) {
  MMG5_pMesh mesh=NULL; MMG5_pSol met=NULL;
  REQUIRE(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  REQUIRE(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_verbose,-1));
  REQUIRE(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_mem,64));
  enum { N=9, NP=N*N, NT=2*(N-1)*(N-1) };
  REQUIRE(MMGS_Set_meshSize(mesh,NP,NT,0));
  REQUIRE(MMGS_Set_solSize(mesh,met,MMG5_Vertex,NP,MMG5_Scalar));
  for(int y=0;y<N;y++) for(int x=0;x<N;x++) {
    int id=y*N+x+1;
    REQUIRE(MMGS_Set_vertex(mesh,x/8.,y/8.,0.,0,id));
    REQUIRE(MMGS_Set_scalarSol(met,0.07+0.18*x/8.,id));
  }
  int tid=1;
  for(int y=0;y<N-1;y++) for(int x=0;x<N-1;x++) {
    int a=y*N+x+1,b=a+1,c=a+N,d=c+1;
    REQUIRE(MMGS_Set_triangle(mesh,a,b,d,1,tid++));
    REQUIRE(MMGS_Set_triangle(mesh,a,d,c,1,tid++));
  }
  REQUIRE(MMGS_Chk_meshData(mesh,met));
  int status=MMGS_mmgslib(mesh,met);
  REQUIRE(status==MMG5_SUCCESS);
  MMG5_int np=0,nt=0,na=0;
  REQUIRE(MMGS_Get_meshSize(mesh,&np,&nt,&na));
  REQUIRE(np>0 && nt>0);
  double* xyz=(double*)calloc((size_t)(np+1)*3,sizeof(double)); REQUIRE(xyz);
  int left=0,right=0;
  for(MMG5_int i=1;i<=np;i++) {
    MMG5_int ref; int corner,required;
    REQUIRE(MMGS_Get_vertex(mesh,xyz+3*i,xyz+3*i+1,xyz+3*i+2,&ref,&corner,&required));
    REQUIRE(isfinite(xyz[3*i]) && isfinite(xyz[3*i+1]) && fabs(xyz[3*i+2])<1e-12);
    left+=fabs(xyz[3*i])<1e-10; right+=fabs(xyz[3*i]-1)<1e-10;
  }
  double area=0.;
  for(MMG5_int i=1;i<=nt;i++) {
    MMG5_int a,b,c,ref; int required;
    REQUIRE(MMGS_Get_triangle(mesh,&a,&b,&c,&ref,&required));
    REQUIRE(a>=1 && a<=np && b>=1 && b<=np && c>=1 && c<=np);
    double q=(xyz[3*b]-xyz[3*a])*(xyz[3*c+1]-xyz[3*a+1])-(xyz[3*b+1]-xyz[3*a+1])*(xyz[3*c]-xyz[3*a]);
    REQUIRE(q>1e-14); area+=0.5*q;
  }
  REQUIRE(fabs(area-1.)<1e-9);
  printf("MMGS_PRODUCTION status=%d vertices=%d triangles=%d before_vertices=%d before_triangles=%d\n",status,(int)np,(int)nt,NP,NT);
  printf("BOUNDARY_OBSERVATION left=%d right=%d area=%.12f\n",left,right,area);
  free(xyz);
  REQUIRE(MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  puts("MMGS_BASELINE_COMPLETE");
  return 0;
}
