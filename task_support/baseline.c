#include "qaws.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static double dot(qaws_vec3 a,qaws_vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static qaws_vec3 sub(qaws_vec3 a,qaws_vec3 b){qaws_vec3 r={a.x-b.x,a.y-b.y,a.z-b.z};return r;}
int main(int argc,char **argv){
 qaws_curve *path=NULL,*profile=NULL,*narrow=NULL; qaws_surface *sweep=NULL,*pipe=NULL;
 qaws_vec3 c[4]={{0,-.125,0},{1./3,.125,0},{2./3,-.125,0},{1,.125,0}};
 qaws_vec2 p[2]={{1,0},{1,.25}};
 qaws_bezier_desc d={QAWS_DIMENSION_3D,3,c,4},dp={QAWS_DIMENSION_2D,1,p,2};
 qaws_surface_swept_desc sd; qaws_surface_eval_result a,b;
 if(qaws_curve_create_bezier(&path,&d)||qaws_curve_create_bezier(&profile,&dp))return 1;
 memset(&sd,0,sizeof sd);sd.path=path;sd.profile=profile;
 if(qaws_surface_create_swept(&sweep,&sd))return 2;
 if(qaws_surface_evaluate(&a,sweep,.5-1e-6,.2,QAWS_SURFACE_EVAL_POSITION)||qaws_surface_evaluate(&b,sweep,.5+1e-6,.2,QAWS_SURFACE_EVAL_POSITION))return 3;
 qaws_vec3 jump=sub(a.position,b.position);double jump_len=sqrt(dot(jump,jump));
 qaws_vec3 cp[9]={{-2,0,0},{-1,0,0},{-.3,.2,.1},{0,1,0},{1,1,1},{1,-1,1},{0,-1,0},{.3,-.2,-.1},{2,0,0}};
 qaws_scalar k[13]={0,0,0,0,.49998,.49999,.5,.50001,.50002,1,1,1,1};
 qaws_bspline_desc bd;memset(&bd,0,sizeof bd);bd.dimension=QAWS_DIMENSION_3D;bd.degree=3;bd.control_points=cp;bd.control_point_count=9;bd.knots=k;bd.knot_count=13;
 if(qaws_curve_create_bspline(&narrow,&bd))return 4;
 qaws_surface_pipe_desc pd={narrow,1,.6};if(qaws_surface_create_pipe(&pipe,&pd))return 5;
 qaws_eval_result_3d x0,x1;
 if(qaws_curve_evaluate_3d(&x0,narrow,0,QAWS_EVAL_FLAG_POSITION)||qaws_curve_evaluate_3d(&x1,narrow,1,QAWS_EVAL_FLAG_POSITION))return 6;
 if(qaws_surface_evaluate(&a,pipe,0,0,QAWS_SURFACE_EVAL_POSITION)||qaws_surface_evaluate(&b,pipe,1,0,QAWS_SURFACE_EVAL_POSITION))return 7;
 qaws_vec3 initial=sub(a.position,x0.position),actual=sub(b.position,x1.position);
 enum {STEPS=1024,COUNT=6*STEPS+1};qaws_scalar *ts=malloc(COUNT*sizeof *ts),*frames=malloc(COUNT*9*sizeof *frames);
 if(!ts||!frames)return 8;ts[0]=0;
 for(unsigned span=0;span<6;span++)for(unsigned j=1;j<=STEPS;j++)ts[span*STEPS+j]=k[3+span]+(k[4+span]-k[3+span])*(qaws_scalar)j/STEPS;
 if(qaws_curve_compute_rotation_minimizing_frames_3d(frames,narrow,ts,&initial,COUNT))return 9;
 qaws_vec3 ref={frames[9*(COUNT-1)+3],frames[9*(COUNT-1)+4],frames[9*(COUNT-1)+5]};
 double cosangle=dot(actual,ref)/sqrt(dot(actual,actual)*dot(ref,ref));if(cosangle>1)cosangle=1;if(cosangle< -1)cosangle=-1;
 double angle=acos(cosangle);printf("OBSERVED frenet_jump=%.9g narrow_pipe_angle=%.9g dense_per_span=%d\n",jump_len,angle,STEPS);
 int legacy=argc>1&&!strcmp(argv[1],"legacy");int bad=!isfinite(angle)||!isfinite(jump_len)||(legacy && (!(jump_len>1.9)||!(angle>.01)));
 free(ts);free(frames);qaws_surface_destroy(pipe);qaws_surface_destroy(sweep);qaws_curve_destroy(narrow);qaws_curve_destroy(profile);qaws_curve_destroy(path);
 if(bad)return 10;
 puts("BASELINE_OK production_sweep_pipe_and_dense_span_reference");return 0;
}
