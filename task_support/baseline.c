#include "par_shapes.h"
#include <math.h>
#include <stdio.h>
int main(void){
 par_shapes_mesh *p=par_shapes_create_plane(3,2),*t=par_shapes_create_torus(12,8,0.35f);
 if(!p||!t||p->npoints!=12||p->ntriangles!=12||!p->tcoords||!p->normals||!t->tcoords)return 1;
 for(int i=0;i<p->npoints;i++) if(!isfinite(p->points[3*i])||fabs(p->normals[3*i+2]-1)>1e-5)return 2;
 int seam=0;for(int a=0;a<t->npoints;a++)for(int b=a+1;b<t->npoints;b++){
 double d=0;for(int k=0;k<3;k++){double z=t->points[3*a+k]-t->points[3*b+k];d+=z*z;}
 if(d<1e-10&&(fabs(t->tcoords[2*a]-t->tcoords[2*b])>0.9||fabs(t->tcoords[2*a+1]-t->tcoords[2*b+1])>0.9))seam++;
 }
 if(seam<12)return 3;
 printf("PAR_BASELINE plane_points=%d plane_faces=%d torus_faces=%d uv_seam_pairs=%d\n",p->npoints,p->ntriangles,t->ntriangles,seam);
 par_shapes_mesh *u=par_shapes_clone(p,0);
 float expected_n[3]={p->normals[3*p->triangles[0]],p->normals[3*p->triangles[0]+1],p->normals[3*p->triangles[0]+2]};
 float expected_uv[2]={p->tcoords[2*p->triangles[0]],p->tcoords[2*p->triangles[0]+1]};
 float *oldn=u->normals,*olduv=u->tcoords;
 par_shapes_unweld(u,true);
 /* Only compare pointers: old arrays have only the old vertex capacity. */
 if(u->npoints!=36||u->normals!=oldn||u->tcoords!=olduv)return 4;
 if(!isfinite(expected_n[2])||!isfinite(expected_uv[0]))return 5;
 par_shapes_mesh *bare=par_shapes_create_cube();
 par_shapes_clone(bare,u);
 if(!u->normals||!u->tcoords)return 6;
 printf("PIPELINE_START unweld_points=%d old_streams_retained=1 clone_missing_streams_stale=1\n",36);
 par_shapes_free_mesh(bare);par_shapes_free_mesh(u);
 par_shapes_free_mesh(p);par_shapes_free_mesh(t);return 0;
}
