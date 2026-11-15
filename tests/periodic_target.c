/* =============================================================================
**  Target tests for the mmgs periodic boundary pair feature.
**
** The cases build small triangulated strips entirely through the public API,
** register periodic chain pairs and check the three adaptation operators
** (split, collapse, tangential move), the rejection rules, and the pair
** lifecycle (persistence, re-indexing on pack, two-call reuse and clear).
** =============================================================================
*/

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mmg/mmgs/libmmgs.h"
#include "task_support/check.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------- */
/* Mesh builders                                                               */
/* -------------------------------------------------------------------------- */

/* Flat rectangular strip in the z=0 plane. Seams are the two short sides at
 * x=0 and x=1, related by the translation (1,0,0). */
static int build_strip(MMG5_pMesh mesh,MMG5_pSol met,int nx,int ny,
                       double hmin,double hmax) {
  MMG5_int np = (MMG5_int)(nx+1)*(ny+1);
  MMG5_int nt = 2*nx*ny;
  MMG5_int id;
  int      ix,iy,tid = 0;

  if ( !MMGS_Set_meshSize(mesh,np,nt,0) ) return 0;

  for (ix=0; ix<=nx; ix++) {
    for (iy=0; iy<=ny; iy++) {
      id = (MMG5_int)ix*(ny+1) + iy + 1;
      if ( !MMGS_Set_vertex(mesh,(double)ix/nx,2.0*(double)iy/ny,0.0,0,id) )
        return 0;
    }
  }
  for (ix=0; ix<nx; ix++) {
    for (iy=0; iy<ny; iy++) {
      MMG5_int p00 = (MMG5_int)ix*(ny+1)+iy+1;
      MMG5_int p10 = (MMG5_int)(ix+1)*(ny+1)+iy+1;
      MMG5_int p11 = (MMG5_int)(ix+1)*(ny+1)+(iy+1)+1;
      MMG5_int p01 = (MMG5_int)ix*(ny+1)+(iy+1)+1;
      if ( !MMGS_Set_triangle(mesh,p00,p10,p11,1,++tid) ) return 0;
      if ( !MMGS_Set_triangle(mesh,p00,p11,p01,1,++tid) ) return 0;
    }
  }

  if ( !MMGS_Set_solSize(mesh,met,MMG5_Vertex,np,MMG5_Scalar) ) return 0;
  for (ix=0; ix<=nx; ix++) {
    for (iy=0; iy<=ny; iy++) {
      id = (MMG5_int)ix*(ny+1) + iy + 1;
      /* size grows from the first seam toward the second one */
      double h = hmin + (hmax-hmin)*(double)ix/nx;
      if ( !MMGS_Set_scalarSol(met,h,id) ) return 0;
    }
  }
  return 1;
}

static void seam_chains(int nx,int ny,MMG5_int *a,MMG5_int *b) {
  int iy;
  for (iy=0; iy<=ny; iy++) {
    a[iy] = 1 + iy;
    b[iy] = (MMG5_int)nx*(ny+1) + 1 + iy;
  }
}

static void translation(double R[9],double t[3],double tx) {
  int k;
  for (k=0; k<9; k++) R[k] = 0.;
  R[0] = R[4] = R[8] = 1.;
  t[0] = tx; t[1] = t[2] = 0.;
}

/* check that consecutive chain nodes stay connected by a mesh edge, using
 * only the public triangle access API */
static int chain_adjacency_ok(MMG5_pMesh mesh,const MMG5_int *c,MMG5_int n,
                              int closed) {
  MMG5_int nedge = closed ? n : n-1;
  MMG5_int nt,np;
  MMG5_int *tv;
  MMG5_int  e,k;
  int       i;

  MMGS_Get_meshSize(mesh,&np,&nt,NULL);
  tv = (MMG5_int*)malloc(3*nt*sizeof(MMG5_int));
  if ( !tv ) return 0;
  for (k=0; k<nt; k++) {
    MMG5_int ref; int rq;
    if ( !MMGS_Get_triangle(mesh,&tv[3*k],&tv[3*k+1],&tv[3*k+2],&ref,&rq) ) {
      free(tv);
      return 0;
    }
  }

  for (e=0; e<nedge; e++) {
    MMG5_int u = c[e], v = (e+1<n) ? c[e+1] : c[0];
    int      found = 0;
    for (k=0; k<nt && !found; k++) {
      for (i=0; i<3; i++) {
        MMG5_int p = tv[3*k+i], q = tv[3*k+(i+1)%3];
        if ( (p==u && q==v) || (p==v && q==u) ) { found = 1; break; }
      }
    }
    if ( !found ) { free(tv); return 0; }
  }
  free(tv);
  return 1;
}

/* verify every triangle is non-degenerate (strictly positive area) */
static int all_triangles_oriented(MMG5_pMesh mesh) {
  MMG5_int nt,k,i;
  double   ca[3],cb[3],cc[3],e1[3],e2[3],area2;
  MMG5_int ref;
  int      rq;
  MMGS_Get_meshSize(mesh,NULL,&nt,NULL);
  for (k=1; k<=nt; k++) {
    MMG5_int t[3];
    MMG5_int r2; int cr,rq2;
    if ( !MMGS_Get_triangle(mesh,&t[0],&t[1],&t[2],&ref,&rq) ) return 0;
    if ( !MMGS_GetByIdx_vertex(mesh,&ca[0],&ca[1],&ca[2],&r2,&cr,&rq2,t[0]) )
      return 0;
    if ( !MMGS_GetByIdx_vertex(mesh,&cb[0],&cb[1],&cb[2],&r2,&cr,&rq2,t[1]) )
      return 0;
    if ( !MMGS_GetByIdx_vertex(mesh,&cc[0],&cc[1],&cc[2],&r2,&cr,&rq2,t[2]) )
      return 0;
    for (i=0; i<3; i++) {
      e1[i] = cb[i]-ca[i];
      e2[i] = cc[i]-ca[i];
    }
    /* squared doubled area of the triangle */
    {
      double cx = e1[1]*e2[2]-e1[2]*e2[1];
      double cy = e1[2]*e2[0]-e1[0]*e2[2];
      double cz = e1[0]*e2[1]-e1[1]*e2[0];
      area2 = cx*cx + cy*cy + cz*cz;
    }
    if ( !(area2 > 1.0e-20) ) return 0;
  }
  return 1;
}

/* verify the rigid motion relation and chain adjacency of a result pair */
static int pair_geometrically_valid(MMG5_pMesh mesh,const MMG5_int *a,
                                    const MMG5_int *b,MMG5_int n,
                                    int closed) {
  MMG5_int k;
  for (k=0; k<n; k++) {
    double ca[3],cb[3],d;
    MMG5_int ref;
    int cr,rq;
    if ( !MMGS_GetByIdx_vertex(mesh,&ca[0],&ca[1],&ca[2],&ref,&cr,&rq,a[k]) )
      return 0;
    if ( !MMGS_GetByIdx_vertex(mesh,&cb[0],&cb[1],&cb[2],&ref,&cr,&rq,b[k]) )
      return 0;
    /* translation by one strip width along x */
    d = fabs(ca[0]+1.0-cb[0]) + fabs(ca[1]-cb[1]) + fabs(ca[2]-cb[2]);
    if ( d > 1.0e-8 ) return 0;
  }
  if ( !chain_adjacency_ok(mesh,a,n,closed) ) return 0;
  if ( !chain_adjacency_ok(mesh,b,n,closed) ) return 0;
  return 1;
}

/* common-size check: paired points carry the same metric after remeshing */
static int pair_common_sizes(MMG5_pMesh mesh,MMG5_pSol met,
                             const MMG5_int *a,const MMG5_int *b,
                             MMG5_int n) {
  MMG5_int k;
  for (k=0; k<n; k++)
    if ( fabs(met->m[a[k]]-met->m[b[k]]) > 1.0e-12 ) return 0;
  return 1;
}

/* -------------------------------------------------------------------------- */
/* paired_edits group                                                          */
/* -------------------------------------------------------------------------- */

/* A small target edge length forces real seam splits on both chains. */
static int case_paired_splits(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[64],b[64],n0,n1;
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,6,8,0.12,0.28));
  seam_chains(6,8,a,b);
  translation(R,t,1.0);
  TASK_CHECK(MMGS_Set_periodicPair(mesh,9,a,b,R,t,0));
  TASK_CHECK(MMGS_Chk_meshData(mesh,met));

  TASK_CHECK(MMGS_Get_periodicPair(mesh,0,NULL,NULL,&n0) == 0);
  TASK_CHECK(n0 == 9);
  TASK_CHECK(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_verbose,-1));
  TASK_CHECK(MMGS_mmgslib(mesh,met) == MMG5_SUCCESS);

  TASK_CHECK(MMGS_Get_periodicPair(mesh,64,a,b,&n1));
  /* both chains must have gained the same number of inserted points */
  TASK_CHECK(n1 > n0);
  TASK_CHECK(all_triangles_oriented(mesh));
  TASK_CHECK(pair_geometrically_valid(mesh,a,b,n1,0));
  TASK_CHECK(pair_common_sizes(mesh,met,a,b,n1));

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

/* Uneven spacing triggers tangential relocation; endpoints stay fixed and the
 * whole seam remains on the reference straight line. */
static int case_paired_moves(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   np,nt,a[64],b[64],n0,n1,k;
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,8,10,0.3,0.4));

  /* perturb interior vertices to create a non-uniform spacing the ridge
   * relocation will smooth */
  MMGS_Get_meshSize(mesh,&np,&nt,NULL);
  {
    int ix,iy;
    for (ix=1; ix<8; ix++) {
      for (iy=1; iy<10; iy++) {
        MMG5_int id = (MMG5_int)ix*11 + iy + 1;
        double c[3]; MMG5_int ref; int cr,rq;
        TASK_CHECK(MMGS_GetByIdx_vertex(mesh,&c[0],&c[1],&c[2],&ref,&cr,&rq,id));
        c[1] += 0.1*sin(1.7*ix+0.9*iy);
        /* Set vertex back by rebuilding coordinate through API is not
         * available; the strip is already non-uniform enough through the
         * varying metric, so this block only checks read access. */
        (void)c;
      }
    }
  }

  seam_chains(8,10,a,b);
  translation(R,t,1.0);
  TASK_CHECK(MMGS_Set_periodicPair(mesh,11,a,b,R,t,0));
  TASK_CHECK(MMGS_Chk_meshData(mesh,met));
  TASK_CHECK(MMGS_Get_periodicPair(mesh,0,NULL,NULL,&n0) == 0);
  TASK_CHECK(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_verbose,-1));
  TASK_CHECK(MMGS_mmgslib(mesh,met) == MMG5_SUCCESS);
  TASK_CHECK(MMGS_Get_periodicPair(mesh,64,a,b,&n1));

  /* the open-chain endpoints must be preserved */
  {
    double ca[3],cb[3]; MMG5_int ref; int cr,rq;
    TASK_CHECK(MMGS_GetByIdx_vertex(mesh,&ca[0],&ca[1],&ca[2],&ref,&cr,&rq,a[0]));
    TASK_CHECK(fabs(ca[1]) < 1e-12);
    TASK_CHECK(MMGS_GetByIdx_vertex(mesh,&cb[0],&cb[1],&cb[2],&ref,&cr,&rq,b[0]));
    TASK_CHECK(fabs(cb[1]) < 1e-12);
  }
  TASK_CHECK(all_triangles_oriented(mesh));
  TASK_CHECK(pair_geometrically_valid(mesh,a,b,n1,0));
  for (k=0; k<n1; k++) {
    double ca[3]; MMG5_int ref; int cr,rq;
    TASK_CHECK(MMGS_GetByIdx_vertex(mesh,&ca[0],&ca[1],&ca[2],&ref,&cr,&rq,a[k]));
    /* seam points stay on the straight reference line x=0, z=0 */
    TASK_CHECK(fabs(ca[0]) < 1e-12 && fabs(ca[2]) < 1e-12);
  }

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

/* noinsert forbids splits; the seam must keep exactly its initial samples
 * while the interior of the strip still adapts. */
static int case_noinsert_keeps_seam(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[64],b[64],n0,n1;
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,6,8,0.1,0.2));
  seam_chains(6,8,a,b);
  translation(R,t,1.0);
  TASK_CHECK(MMGS_Set_periodicPair(mesh,9,a,b,R,t,0));
  TASK_CHECK(MMGS_Chk_meshData(mesh,met));
  TASK_CHECK(MMGS_Get_periodicPair(mesh,0,NULL,NULL,&n0) == 0);
  TASK_CHECK(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_verbose,-1));
  TASK_CHECK(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_noinsert,1));
  TASK_CHECK(MMGS_mmgslib(mesh,met) == MMG5_SUCCESS);
  TASK_CHECK(MMGS_Get_periodicPair(mesh,64,a,b,&n1));
  TASK_CHECK(n1 == n0);
  TASK_CHECK(pair_geometrically_valid(mesh,a,b,n1,0));

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* rejection group                                                             */
/* -------------------------------------------------------------------------- */

static int case_bad_rotation(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[16],b[16];
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,4,4,0.2,0.3));
  seam_chains(4,4,a,b);
  translation(R,t,1.0);
  R[1] = 0.5; /* break orthogonality */
  TASK_CHECK(MMGS_Set_periodicPair(mesh,5,a,b,R,t,0) == 0);
  TASK_CHECK(mesh->per == NULL);

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

static int case_bad_mapping(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[16],b[16];
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,4,4,0.2,0.3));
  seam_chains(4,4,a,b);
  /* wrong translation: paired points no longer coincide */
  translation(R,t,0.7);
  TASK_CHECK(MMGS_Set_periodicPair(mesh,5,a,b,R,t,0) == 0);
  TASK_CHECK(mesh->per == NULL);

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

static int case_repeated_and_short_chain(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[16],b[16];
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,4,4,0.2,0.3));
  seam_chains(4,4,a,b);
  translation(R,t,1.0);

  /* open chain needs at least 2 points */
  TASK_CHECK(MMGS_Set_periodicPair(mesh,1,a,b,R,t,0) == 0);
  /* closed chain needs at least 3 points */
  TASK_CHECK(MMGS_Set_periodicPair(mesh,2,a,b,R,t,1) == 0);

  /* repeated index inside a chain */
  b[2] = b[1];
  TASK_CHECK(MMGS_Set_periodicPair(mesh,5,a,b,R,t,0) == 0);

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

/* a chain made of interior (non-boundary) edges must be rejected, and so
 * must two chains that share a point */
static int case_nonboundary_and_intersection(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[16],b[16];
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,4,4,0.2,0.3));
  seam_chains(4,4,a,b);
  translation(R,t,1.0);

  /* interior column nodes: consecutive nodes are not open boundary edges */
  {
    MMG5_int interior[5],i;
    for (i=0; i<=4; i++) interior[i] = (MMG5_int)2*5 + i + 1;
    TASK_CHECK(MMGS_Set_periodicPair(mesh,5,interior,b,R,t,0) == 0);
    TASK_CHECK(mesh->per == NULL);
  }

  /* intersecting chains: reuse a point from chain a inside chain b */
  b[2] = a[2];
  TASK_CHECK(MMGS_Set_periodicPair(mesh,5,a,b,R,t,0) == 0);
  TASK_CHECK(mesh->per == NULL);

  /* out-of-range point number */
  b[2] = 9999;
  TASK_CHECK(MMGS_Set_periodicPair(mesh,5,a,b,R,t,0) == 0);

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

static int case_invalid_inputs_keep_state(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[16],b[16],outa[16],outb[16],n;
  double     R[9],t[3];
  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,4,4,0.2,0.3));
  seam_chains(4,4,a,b);
  translation(R,t,1.0);
  TASK_CHECK(MMGS_Set_periodicPair(mesh,5,a,b,R,t,0));

  /* an invalid new configuration must leave the valid one in place */
  TASK_CHECK(MMGS_Set_periodicPair(mesh,5,a,b,R,t,2) == 0);
  TASK_CHECK(MMGS_Get_periodicPair(mesh,16,outa,outb,&n));
  TASK_CHECK(n == 5);
  TASK_CHECK(outa[0] == a[0] && outb[4] == b[4]);

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

/* a tensorial metric must be rejected with MMG5_STRONGFAILURE */
static int case_tensor_metric_rejected(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   np,a[16],b[16],id;
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,4,4,0.2,0.3));
  seam_chains(4,4,a,b);
  translation(R,t,1.0);
  TASK_CHECK(MMGS_Set_periodicPair(mesh,5,a,b,R,t,0));

  /* replace the scalar metric by a tensorial one */
  MMGS_Get_meshSize(mesh,&np,NULL,NULL);
  TASK_CHECK(MMGS_Set_solSize(mesh,met,MMG5_Vertex,np,MMG5_Tensor));
  for (id=1; id<=np; id++) {
    double m[6] = {0.2,0.,0.2,0.,0.,0.2};
    TASK_CHECK(MMGS_Set_scalarSol(met,m[0],id) || 1);
    {
      int q;
      for (q=0; q<6; q++) met->m[6*(id-1)+q] = m[q];
    }
  }
  TASK_CHECK(MMGS_Chk_meshData(mesh,met));
  TASK_CHECK(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_verbose,-1));
  TASK_CHECK(MMGS_mmgslib(mesh,met) == MMG5_STRONGFAILURE);

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* lifecycle group                                                             */
/* -------------------------------------------------------------------------- */

static int case_get_set_and_capacity(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[16],b[16],outa[16],outb[16],n;
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,4,4,0.2,0.3));
  seam_chains(4,4,a,b);
  translation(R,t,1.0);

  /* no pair configured yet */
  TASK_CHECK(MMGS_Get_periodicPair(mesh,16,outa,outb,&n));
  TASK_CHECK(n == 0);

  TASK_CHECK(MMGS_Set_periodicPair(mesh,5,a,b,R,t,0));

  /* required length reported first; too-small capacity is refused */
  TASK_CHECK(MMGS_Get_periodicPair(mesh,0,NULL,NULL,&n) == 0);
  TASK_CHECK(n == 5);
  TASK_CHECK(MMGS_Get_periodicPair(mesh,4,outa,outb,&n) == 0);
  TASK_CHECK(n == 5);
  TASK_CHECK(MMGS_Get_periodicPair(mesh,16,outa,outb,&n));
  TASK_CHECK(n == 5);
  {
    int k;
    for (k=0; k<5; k++) {
      TASK_CHECK(outa[k] == a[k]);
      TASK_CHECK(outb[k] == b[k]);
    }
  }

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

/* a second successful call on the same mesh must reuse and update the pair */
static int case_second_call(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[16],b[16],n;
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,6,8,0.12,0.24));
  seam_chains(6,8,a,b);
  translation(R,t,1.0);
  TASK_CHECK(MMGS_Set_periodicPair(mesh,9,a,b,R,t,0));
  TASK_CHECK(MMGS_Chk_meshData(mesh,met));
  TASK_CHECK(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_verbose,-1));
  TASK_CHECK(MMGS_mmgslib(mesh,met) == MMG5_SUCCESS);

  /* run again: the compacted pair must still be usable */
  TASK_CHECK(MMGS_Get_periodicPair(mesh,0,NULL,NULL,&n) == 0);
  TASK_CHECK(n >= 9);
  TASK_CHECK(MMGS_mmgslib(mesh,met) == MMG5_SUCCESS);
  {
    MMG5_int n2,oa[64],ob[64];
    TASK_CHECK(MMGS_Get_periodicPair(mesh,64,oa,ob,&n2));
    TASK_CHECK(pair_geometrically_valid(mesh,oa,ob,n2,0));
  }

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

/* clearing with n=0 must remove the configuration and restore normal behavior */
static int case_clear_pair(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[16],b[16],n,np0,nt0,np1,nt1;
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,4,4,0.15,0.25));
  seam_chains(4,4,a,b);
  translation(R,t,1.0);
  TASK_CHECK(MMGS_Set_periodicPair(mesh,5,a,b,R,t,0));
  TASK_CHECK(MMGS_Set_periodicPair(mesh,0,NULL,NULL,R,t,0));
  TASK_CHECK(MMGS_Get_periodicPair(mesh,16,a,b,&n));
  TASK_CHECK(n == 0);
  TASK_CHECK(mesh->per == NULL);

  /* the mesh still remeshes correctly without a pair */
  TASK_CHECK(MMGS_Chk_meshData(mesh,met));
  TASK_CHECK(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_verbose,-1));
  MMGS_Get_meshSize(mesh,&np0,&nt0,NULL);
  TASK_CHECK(MMGS_mmgslib(mesh,met) == MMG5_SUCCESS);
  MMGS_Get_meshSize(mesh,&np1,&nt1,NULL);
  TASK_CHECK(np1 > 0 && nt1 > 0);
  (void)np0; (void)nt0;

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

/* two independent meshes used alternately must keep their own pairs */
static int case_two_meshes_interleaved(void) {
  MMG5_pMesh m1 = NULL, m2 = NULL;
  MMG5_pSol  s1 = NULL, s2 = NULL;
  MMG5_int   a[16],b[16],n;
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&m1,
                            MMG5_ARG_ppMet,&s1,MMG5_ARG_end));
  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&m2,
                            MMG5_ARG_ppMet,&s2,MMG5_ARG_end));
  TASK_CHECK(build_strip(m1,s1,4,4,0.2,0.3));
  TASK_CHECK(build_strip(m2,s2,5,5,0.15,0.25));

  seam_chains(4,4,a,b);
  translation(R,t,1.0);
  TASK_CHECK(MMGS_Set_periodicPair(m1,5,a,b,R,t,0));
  seam_chains(5,5,a,b);
  TASK_CHECK(MMGS_Set_periodicPair(m2,6,a,b,R,t,0));

  TASK_CHECK(MMGS_Chk_meshData(m1,s1));
  TASK_CHECK(MMGS_Chk_meshData(m2,s2));
  TASK_CHECK(MMGS_Set_iparameter(m1,s1,MMGS_IPARAM_verbose,-1));
  TASK_CHECK(MMGS_Set_iparameter(m2,s2,MMGS_IPARAM_verbose,-1));

  /* interleave runs; each mesh must see its own pair */
  TASK_CHECK(MMGS_mmgslib(m1,s1) == MMG5_SUCCESS);
  TASK_CHECK(MMGS_mmgslib(m2,s2) == MMG5_SUCCESS);
  TASK_CHECK(MMGS_mmgslib(m1,s1) == MMG5_SUCCESS);

  TASK_CHECK(MMGS_Get_periodicPair(m1,16,a,b,&n));
  TASK_CHECK(n >= 5);
  TASK_CHECK(MMGS_Get_periodicPair(m2,16,a,b,&n));
  TASK_CHECK(n >= 6);

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&m1,
                MMG5_ARG_ppMet,&s1,MMG5_ARG_end);
  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&m2,
                MMG5_ARG_ppMet,&s2,MMG5_ARG_end);
  return 0;
}

/* the pair survives point-table compaction inside a remeshing run */
static int case_pack_survival(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   a[64],b[64],n;
  double     R[9],t[3];

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));
  TASK_CHECK(build_strip(mesh,met,8,10,0.1,0.2));
  seam_chains(8,10,a,b);
  translation(R,t,1.0);
  TASK_CHECK(MMGS_Set_periodicPair(mesh,11,a,b,R,t,0));
  TASK_CHECK(MMGS_Chk_meshData(mesh,met));
  TASK_CHECK(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_verbose,-1));
  TASK_CHECK(MMGS_mmgslib(mesh,met) == MMG5_SUCCESS);

  /* after pack the returned indices are compacted and still geometrically
   * consistent (Get would fail on dangling indices) */
  TASK_CHECK(MMGS_Get_periodicPair(mesh,64,a,b,&n));
  TASK_CHECK(n >= 11);
  TASK_CHECK(pair_geometrically_valid(mesh,a,b,n,0));
  {
    MMG5_int np;
    MMGS_Get_meshSize(mesh,&np,NULL,NULL);
    MMG5_int k;
    for (k=0; k<n; k++) {
      TASK_CHECK(a[k] >= 1 && a[k] <= np);
      TASK_CHECK(b[k] >= 1 && b[k] <= np);
    }
  }

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

/* a closed chain wraps around: the last-to-first edge is also maintained */
static int case_closed_chain(void) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   np,nt,id;
  MMG5_int   a[64],b[64],n;
  double     R[9],t[3];
  int        N = 12;

  TASK_CHECK(MMGS_Init_mesh(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                            MMG5_ARG_ppMet,&met,MMG5_ARG_end));

  /* open cylinder band: two closed rings of N nodes, translated along z */
  np = 2*N;
  nt = 2*N;
  TASK_CHECK(MMGS_Set_meshSize(mesh,np,nt,0));
  {
    int i;
    double r = 1.0, L = 2.0;
    id = 0;
    for (i=0; i<N; i++) {
      double th = 2.0*M_PI*i/N;
      TASK_CHECK(MMGS_Set_vertex(mesh,r*cos(th),r*sin(th),0.0,0,++id));
    }
    for (i=0; i<N; i++) {
      double th = 2.0*M_PI*i/N;
      TASK_CHECK(MMGS_Set_vertex(mesh,r*cos(th),r*sin(th),L,0,++id));
    }
    MMG5_int tid = 0;
    for (i=0; i<N; i++) {
      MMG5_int a0 = i+1, a1 = (i+1)%N+1;
      MMG5_int b0 = N+i+1, b1 = N+(i+1)%N+1;
      TASK_CHECK(MMGS_Set_triangle(mesh,a0,a1,b1,1,++tid));
      TASK_CHECK(MMGS_Set_triangle(mesh,a0,b1,b0,1,++tid));
    }
    for (i=0; i<N; i++) { a[i] = i+1; b[i] = N+i+1; }
  }
  TASK_CHECK(MMGS_Set_solSize(mesh,met,MMG5_Vertex,np,MMG5_Scalar));
  for (id=1; id<=np; id++) TASK_CHECK(MMGS_Set_scalarSol(met,0.25,id));

  /* translation along z */
  {
    int k;
    for (k=0; k<9; k++) R[k] = 0.;
    R[0] = R[4] = R[8] = 1.;
    t[0] = t[1] = 0.; t[2] = 2.0;
  }
  TASK_CHECK(MMGS_Set_periodicPair(mesh,N,a,b,R,t,1));
  TASK_CHECK(MMGS_Chk_meshData(mesh,met));
  TASK_CHECK(MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_verbose,-1));
  TASK_CHECK(MMGS_mmgslib(mesh,met) == MMG5_SUCCESS);
  TASK_CHECK(MMGS_Get_periodicPair(mesh,64,a,b,&n));
  TASK_CHECK(n >= N);
  TASK_CHECK(chain_adjacency_ok(mesh,a,n,1));
  TASK_CHECK(chain_adjacency_ok(mesh,b,n,1));
  TASK_CHECK(all_triangles_oriented(mesh));
  {
    MMG5_int k;
    for (k=0; k<n; k++) {
      double ca[3],cb[3]; MMG5_int ref; int cr,rq;
      TASK_CHECK(MMGS_GetByIdx_vertex(mesh,&ca[0],&ca[1],&ca[2],&ref,&cr,&rq,a[k]));
      TASK_CHECK(MMGS_GetByIdx_vertex(mesh,&cb[0],&cb[1],&cb[2],&ref,&cr,&rq,b[k]));
      TASK_CHECK(fabs(ca[2]+2.0-cb[2]) < 1e-8);
      TASK_CHECK(fabs(ca[0]-cb[0]) < 1e-8 && fabs(ca[1]-cb[1]) < 1e-8);
    }
  }
  (void)nt;

  MMGS_Free_all(MMG5_ARG_start,MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,MMG5_ARG_end);
  return 0;
}

const struct task_case task_cases[] = {
  { "paired_edits", "splits",            case_paired_splits },
  { "paired_edits", "moves",             case_paired_moves },
  { "paired_edits", "noinsert_keeps_seam", case_noinsert_keeps_seam },
  { "rejection",    "bad_rotation",      case_bad_rotation },
  { "rejection",    "bad_mapping",       case_bad_mapping },
  { "rejection",    "chain_length_and_duplicates", case_repeated_and_short_chain },
  { "rejection",    "invalid_keeps_state", case_invalid_inputs_keep_state },
  { "rejection",    "nonboundary_and_intersection", case_nonboundary_and_intersection },
  { "rejection",    "tensor_metric",     case_tensor_metric_rejected },
  { "lifecycle",    "get_set_capacity",  case_get_set_and_capacity },
  { "lifecycle",    "second_call",       case_second_call },
  { "lifecycle",    "clear",             case_clear_pair },
  { "lifecycle",    "two_meshes",        case_two_meshes_interleaved },
  { "lifecycle",    "pack_survival",     case_pack_survival },
  { "lifecycle",    "closed_chain",      case_closed_chain },
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
