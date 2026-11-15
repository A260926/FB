/* =============================================================================
**  This file is part of the mmg software package for the tetrahedral
**  mesh modification.
**  Copyright (c) Bx INP/CNRS/Inria/UBordeaux/UPMC, 2004-
**
**  mmg is free software: you can redistribute it and/or modify it
**  under the terms of the GNU Lesser General Public License as published
**  by the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  mmg is distributed in the hope that it will be useful, but WITHOUT
**  ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
**  Lesser General Public License for more details.
**
**  You should have received a copy of the GNU Lesser General Public
**  License and of the GNU General Public License along with mmg (in
**  files COPYING.LESSER and COPYING). If not, see
**  <http://www.gnu.org/licenses/>. Please read their terms carefully and
**  use this copy of the mmg distribution only if you accept them.
** =============================================================================
*/

/**
 * \file mmgs/periodic_s.c
 * \brief Periodic boundary pair handling for the surface remesher.
 *
 * A periodic pair describes two boundary chains of one surface mesh whose
 * samples match through a rigid motion x_b = R x_a + t. The chains take part
 * in the ordinary adaptation operators (edge split, edge collapse and
 * tangential relocation) but every operator is committed on both chains at
 * once: a candidate has to pass the geometry, quality and topology checks
 * of both sides, otherwise neither side is modified.
 *
 * This file holds the public MMGS_Set_periodicPair / MMGS_Get_periodicPair
 * API and the internal hooks called from the mmgs adaptation pipeline.
 */

#include <math.h>
#include <float.h>

#include "libmmgs.h"
#include "libmmgs_private.h"
#include "mmgsexterns_private.h"
#include "mmgexterns_private.h"
#include "inlined_functions_private.h"

/** Tolerance on the rigid motion relation. */
#define MMGS_PER_EPS        1.0e-10
/** Tolerance for the projection of a moved/inserted point on its chain. */
#define MMGS_PER_PROJ_EPS   1.0e-8
/** Orthogonality tolerance accepted for the input rotation. */
#define MMGS_PER_ROT_EPS    1.0e-8
/** Tolerance on the rigid motion relation in the normalized frame. */
#define MMGS_PER_MAP_EPS    1.0e-9

/**
 * Runtime state of the pair during one remeshing run. The persistent
 * configuration lives in mesh->per; this structure caches the data needed
 * by the adaptation hooks.
 */
typedef struct {
  MMG5_int   n;            /* number of nodes per chain */
  MMG5_int   namax;        /* allocated capacity of the node arrays */
  MMG5_int  *a;            /* chain A node indices (chain order) */
  MMG5_int  *b;            /* chain B node indices (chain order) */
  MMG5_int  *eta;          /* triangle incident to each edge of chain A */
  MMG5_int  *etb;          /* triangle incident to each edge of chain B */
  MMG5_int  *pos;          /* 1-based chain slot of a point, 0 outside */
  double    *refa;         /* reference polyline A, 3 coords per node */
  double    *refb;         /* reference polyline B, 3 coords per node */
  double     R[9];         /* rotation, row major (normalized frame) */
  double     t[3];         /* translation (normalized frame) */
  int8_t     closed;       /* closed chain flag */
  MMG5_pMesh owner;        /* mesh this runtime state belongs to */
} MMGS_PerRuntime;

/*
 * At most one pair is handled per mesh and the remeshing pipeline is
 * sequential: the runtime state is kept in a file scope variable. It is
 * created in MMGS_periodic_begin and released in MMGS_periodic_end, so two
 * meshes used alternately each rebuild their own state from their persistent
 * mesh->per configuration.
 */
static MMGS_PerRuntime *perRun = NULL;

/* -------------------------------------------------------------------------- */
/* Small algebra helpers                                                      */
/* -------------------------------------------------------------------------- */

static int per_finite3(const double *x) {
  int i;
  for (i=0; i<3; i++) if ( !isfinite(x[i]) ) return 0;
  return 1;
}

static int per_finite9(const double *x) {
  int i;
  for (i=0; i<9; i++) if ( !isfinite(x[i]) ) return 0;
  return 1;
}

static void per_apply(const double R[9],const double t[3],
                      const double *x,double *y) {
  y[0] = R[0]*x[0] + R[1]*x[1] + R[2]*x[2] + t[0];
  y[1] = R[3]*x[0] + R[4]*x[1] + R[5]*x[2] + t[1];
  y[2] = R[6]*x[0] + R[7]*x[1] + R[8]*x[2] + t[2];
}

static double per_dist3(const double *x,const double *y) {
  double d0=x[0]-y[0],d1=x[1]-y[1],d2=x[2]-y[2];
  return sqrt(d0*d0+d1*d1+d2*d2);
}

/* R must be finite, orthogonal and have a positive determinant. */
static int per_checkRotation(const double R[9]) {
  double det,err;
  int    i,j,k;

  if ( !per_finite9(R) ) return 0;

  for (i=0; i<3; i++) {
    for (j=0; j<3; j++) {
      double s = 0.;
      for (k=0; k<3; k++) s += R[3*k+i]*R[3*k+j];
      err = fabs(s - (i==j ? 1.0 : 0.0));
      if ( err > MMGS_PER_ROT_EPS ) return 0;
    }
  }

  det = R[0]*(R[4]*R[8]-R[5]*R[7])
      - R[1]*(R[3]*R[8]-R[5]*R[6])
      + R[2]*(R[3]*R[7]-R[4]*R[6]);
  if ( !isfinite(det) || det <= 0. ) return 0;

  return 1;
}

/* -------------------------------------------------------------------------- */
/* Persistent configuration                                                    */
/* -------------------------------------------------------------------------- */

static void per_freePair(MMG5_pMesh mesh) {
  MMG5_pPeriodicPair per = mesh->per;
  if ( !per ) return;
  if ( per->a ) MMG5_DEL_MEM(mesh,per->a);
  if ( per->b ) MMG5_DEL_MEM(mesh,per->b);
  MMG5_SAFE_FREE(per);
  mesh->per = NULL;
}

/* Called when the mesh structure is destroyed. */
void MMGS_periodic_free(MMG5_pMesh mesh) {
  if ( perRun && perRun->owner == mesh ) {
    /* a run was left open (failure path): drop its runtime state as well */
    if (perRun->a) MMG5_SAFE_FREE(perRun->a);
    if (perRun->b) MMG5_SAFE_FREE(perRun->b);
    if (perRun->eta) MMG5_SAFE_FREE(perRun->eta);
    if (perRun->etb) MMG5_SAFE_FREE(perRun->etb);
    if (perRun->pos) MMG5_SAFE_FREE(perRun->pos);
    if (perRun->refa) MMG5_SAFE_FREE(perRun->refa);
    if (perRun->refb) MMG5_SAFE_FREE(perRun->refb);
    MMG5_SAFE_FREE(perRun);
    perRun = NULL;
  }
  per_freePair(mesh);
}

/* -------------------------------------------------------------------------- */
/* Edge validation through a temporary edge hash                              */
/* -------------------------------------------------------------------------- */

/*
 * Check that every edge of the supplied chain is a real open edge of the
 * triangulation (exactly one incident triangle).
 */
static int per_checkChainEdges(MMG5_pMesh mesh,const MMG5_int *chain,
                               MMG5_int n,int closed) {
  MMG5_Hash  hash;
  MMG5_pTria pt;
  MMG5_int   k,i,i1,i2,e,nedge;
  MMG5_int  *count;
  int        ier;

  nedge = closed ? n : n-1;

  if ( !MMG5_hashNew(mesh,&hash,MG_MAX(2*mesh->nt,1),6*mesh->nt+1) )
    return 0;

  /* store the 1-based chain edge index in the hash */
  for (e=0; e<nedge; e++) {
    MMG5_int u = chain[e];
    MMG5_int v = (e+1<n) ? chain[e+1] : chain[0];
    if ( MMG5_hashEdge(mesh,&hash,u,v,e+1) == 0 ) {
      MMG5_DEL_MEM(mesh,hash.item);
      return 0;
    }
  }

  MMG5_SAFE_MALLOC(count,nedge+1,MMG5_int,
                   MMG5_DEL_MEM(mesh,hash.item); return 0);
  for (e=0; e<=nedge; e++) count[e] = 0;

  for (k=1; k<=mesh->nt; k++) {
    pt = &mesh->tria[k];
    if ( !MG_EOK(pt) ) continue;
    for (i=0; i<3; i++) {
      i1 = MMG5_inxt2[i];
      i2 = MMG5_iprv2[i];
      e  = MMG5_hashGet(&hash,pt->v[i1],pt->v[i2]);
      if ( e > 0 ) count[e]++;
    }
  }

  ier = 1;
  for (e=1; e<=nedge; e++) {
    if ( count[e] != 1 ) { ier = 0; break; }
  }

  MMG5_SAFE_FREE(count);
  MMG5_DEL_MEM(mesh,hash.item);
  return ier;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

int MMGS_Set_periodicPair(MMG5_pMesh mesh,MMG5_int n,
                          const MMG5_int *a,const MMG5_int *b,
                          const double *R,const double *t,int closed) {
  MMG5_pPeriodicPair per,old;
  double             diag,tol,d;
  MMG5_int           i,k;
  MMG5_int           namax;

  if ( !mesh || !mesh->point || !mesh->tria ) return 0;
  if ( closed != 0 && closed != 1 ) return 0;

  /* n == 0 clears the configuration */
  if ( n == 0 ) {
    per_freePair(mesh);
    return 1;
  }

  if ( !a || !b || !R || !t ) return 0;

  if ( n < (closed ? 3 : 2) ) return 0;
  if ( n > mesh->np ) return 0;

  if ( !per_finite9(R) || !per_checkRotation(R) ) return 0;
  if ( !per_finite3(t) ) return 0;

  /* point indices validity */
  for (i=0; i<n; i++) {
    if ( a[i] < 1 || a[i] > mesh->np || !MG_VOK(&mesh->point[a[i]]) )
      return 0;
    if ( b[i] < 1 || b[i] > mesh->np || !MG_VOK(&mesh->point[b[i]]) )
      return 0;
    if ( !per_finite3(mesh->point[a[i]].c) ||
         !per_finite3(mesh->point[b[i]].c) )
      return 0;
  }

  /* no repeated index inside a chain and the two chains cannot intersect */
  for (i=0; i<n; i++) {
    for (k=i+1; k<n; k++)
      if ( a[i] == a[k] || b[i] == b[k] ) return 0;
    for (k=0; k<n; k++)
      if ( a[i] == b[k] ) return 0;
  }

  /* bounding box diagonal */
  diag = 0.;
  for (i=0; i<mesh->dim; i++) {
    d = mesh->info.max[i] - mesh->info.min[i];
    if ( d > diag ) diag = d;
  }
  if ( !(diag > 0.) ) {
    double mn[3],mx[3];
    int    j;
    for (j=0; j<3; j++) { mn[j]=DBL_MAX; mx[j]=-DBL_MAX; }
    for (k=1; k<=mesh->np; k++) {
      if ( !MG_VOK(&mesh->point[k]) ) continue;
      for (j=0; j<3; j++) {
        if ( mesh->point[k].c[j] < mn[j] ) mn[j] = mesh->point[k].c[j];
        if ( mesh->point[k].c[j] > mx[j] ) mx[j] = mesh->point[k].c[j];
      }
    }
    diag = 0.;
    for (j=0; j<3; j++) {
      d = mx[j]-mn[j];
      if ( d > diag ) diag = d;
    }
  }
  tol = MMGS_PER_EPS * MG_MAX(1.0,diag);

  /* paired points must match through the rigid motion */
  for (i=0; i<n; i++) {
    double y[3];
    per_apply(R,t,mesh->point[a[i]].c,y);
    if ( per_dist3(y,mesh->point[b[i]].c) > tol ) return 0;
  }

  /* chain edges must be real open edges of the triangulation */
  if ( !per_checkChainEdges(mesh,a,n,closed) ) return 0;
  if ( !per_checkChainEdges(mesh,b,n,closed) ) return 0;

  /* build the new configuration before replacing the old one */
  MMG5_SAFE_CALLOC(per,1,MMG5_PeriodicPair,return 0);
  namax = n + 8;
  MMG5_ADD_MEM(mesh,2*(namax+1)*sizeof(MMG5_int),"periodic pair chains",
               MMG5_SAFE_FREE(per); return 0);
  MMG5_SAFE_CALLOC(per->a,namax+1,MMG5_int,
                   MMG5_SAFE_FREE(per); return 0);
  MMG5_SAFE_CALLOC(per->b,namax+1,MMG5_int,
                   MMG5_DEL_MEM(mesh,per->a);
                   MMG5_SAFE_FREE(per); return 0);
  per->namax = namax;

  for (i=0; i<n; i++) {
    per->a[i+1] = a[i];
    per->b[i+1] = b[i];
  }
  per->n = n;
  memcpy(per->R,R,9*sizeof(double));
  memcpy(per->t,t,3*sizeof(double));
  per->closed = (int8_t)closed;
  per->active = 0;

  old = mesh->per;
  mesh->per = per;
  if ( old ) {
    if ( old->a ) MMG5_DEL_MEM(mesh,old->a);
    if ( old->b ) MMG5_DEL_MEM(mesh,old->b);
    MMG5_SAFE_FREE(old);
  }

  return 1;
}

int MMGS_Get_periodicPair(MMG5_pMesh mesh,MMG5_int capacity,
                          MMG5_int *a,MMG5_int *b,MMG5_int *n) {
  MMG5_pPeriodicPair per;
  MMG5_int           i;

  if ( !mesh || !n ) return 0;

  per = mesh->per;
  if ( !per ) {
    *n = 0;
    return 1;
  }

  *n = per->n;
  if ( capacity < per->n ) return 0;
  if ( !a || !b ) return 0;

  for (i=0; i<per->n; i++) {
    a[i] = per->a[i+1];
    b[i] = per->b[i+1];
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Runtime state                                                               */
/* -------------------------------------------------------------------------- */

int MMGS_periodic_active(MMG5_pMesh mesh) {
  return (mesh && mesh->per && perRun && perRun->owner == mesh) ? 1 : 0;
}

static MMG5_int per_nedge(const MMGS_PerRuntime *rt) {
  return rt->closed ? rt->n : rt->n-1;
}

static MMG5_int per_next(const MMGS_PerRuntime *rt,MMG5_int j) {
  return (j+1 < rt->n) ? j+1 : (rt->closed ? 0 : j);
}

static MMG5_int per_prev(const MMGS_PerRuntime *rt,MMG5_int j) {
  return (j > 0) ? j-1 : (rt->closed ? rt->n-1 : 0);
}

/* rebuild the point -> chain slot table */
static int per_buildPos(MMG5_pMesh mesh) {
  MMGS_PerRuntime *rt = perRun;
  MMG5_int         j;

  if ( rt->pos ) MMG5_SAFE_FREE(rt->pos);
  MMG5_ADD_MEM(mesh,(mesh->npmax+1)*sizeof(MMG5_int),
               "periodic position table",return 0);
  MMG5_SAFE_CALLOC(rt->pos,mesh->npmax+1,MMG5_int,return 0);
  for (j=0; j<rt->n; j++) {
    rt->pos[rt->a[j]] = j+1;
    rt->pos[rt->b[j]] = j+1;
  }
  return 1;
}

/* find one triangle incident to each chain edge */
static int per_buildEdgeTria(MMG5_pMesh mesh) {
  MMGS_PerRuntime *rt = perRun;
  MMG5_pTria       pt;
  MMG5_int         k,i,j,nedge;

  nedge = per_nedge(rt);
  for (j=0; j<nedge; j++) { rt->eta[j] = 0; rt->etb[j] = 0; }

  for (k=1; k<=mesh->nt; k++) {
    pt = &mesh->tria[k];
    if ( !MG_EOK(pt) ) continue;
    for (i=0; i<3; i++) {
      int       i1 = MMG5_inxt2[i];
      int       i2 = MMG5_iprv2[i];
      MMG5_int  u = pt->v[i1];
      MMG5_int  v = pt->v[i2];
      for (j=0; j<nedge; j++) {
        MMG5_int jn = per_next(rt,j);
        if ( !rt->eta[j] &&
             ((rt->a[j]==u && rt->a[jn]==v) || (rt->a[j]==v && rt->a[jn]==u)) )
          rt->eta[j] = k;
        if ( !rt->etb[j] &&
             ((rt->b[j]==u && rt->b[jn]==v) || (rt->b[j]==v && rt->b[jn]==u)) )
          rt->etb[j] = k;
      }
    }
  }
  for (j=0; j<nedge; j++)
    if ( !rt->eta[j] || !rt->etb[j] ) return 0;
  return 1;
}

/* Find the packed edge index (3*k+i) of open edge (ia,ib). */
static MMG5_int per_findPackedEdge(MMG5_pMesh mesh,MMG5_int ia,MMG5_int ib) {
  MMG5_pTria pt;
  MMG5_int   k;
  int        i,i1,i2;

  for (k=1; k<=mesh->nt; k++) {
    pt = &mesh->tria[k];
    if ( !MG_EOK(pt) ) continue;
    for (i=0; i<3; i++) {
      i1 = MMG5_inxt2[i];
      i2 = MMG5_iprv2[i];
      if ( (pt->v[i1]==ia && pt->v[i2]==ib) ||
           (pt->v[i1]==ib && pt->v[i2]==ia) )
        return 3*k+i;
    }
  }
  return 0;
}

/*
 * Return the local edge index of triangle k for which the inxt vertex (the
 * node chkcol/colver removes) is p, restricted to an edge whose other
 * extremity is q (the survivor of the collapse). -1 if none exists.
 */
static int8_t per_localEdgeRemoving(MMG5_pMesh mesh,MMG5_int k,
                                    MMG5_int p,MMG5_int q) {
  int8_t r;
  for (r=0; r<3; r++) {
    int8_t r1 = MMG5_inxt2[r];
    int8_t r2 = MMG5_iprv2[r];
    MMG5_int e1 = mesh->tria[k].v[r1];
    MMG5_int e2 = mesh->tria[k].v[r2];
    if ( e1 != p ) continue;
    /* the oriented edge (e1,e2) must be the chain edge: q is one of the two
     * other vertices of the triangle */
    if ( e2 == q || mesh->tria[k].v[r] == q ) return r;
  }
  return -1;
}

/*
 * Start a remeshing run. The mesh and metric are already normalized. Snapshot
 * the reference polylines, express the rigid motion in the normalized frame
 * and build the runtime lookup tables.
 */
int MMGS_periodic_begin(MMG5_pMesh mesh,MMG5_pSol met) {
  MMG5_pPeriodicPair per;
  MMGS_PerRuntime   *rt;
  MMG5_int           j,namax;
  double             dd,s[3];

  if ( !mesh->per ) return 1;
  per = mesh->per;

  /* only finite positive scalar target edge lengths are allowed */
  if ( !met || met->size != 1 || !met->m || met->np != mesh->np )
    return 0;
  for (j=1; j<=mesh->np; j++)
    if ( !isfinite(met->m[j]) || met->m[j] <= 0. ) return 0;

  MMG5_SAFE_CALLOC(rt,1,MMGS_PerRuntime,return 0);
  perRun = rt;
  rt->owner = mesh;
  rt->n = per->n;
  rt->closed = per->closed;
  namax = per->n + 8;
  rt->namax = namax;

  MMG5_ADD_MEM(mesh,namax*(4*sizeof(MMG5_int)+6*sizeof(double)),
               "periodic runtime state",goto fail);
  MMG5_SAFE_CALLOC(rt->a,namax+1,MMG5_int,goto fail);
  MMG5_SAFE_CALLOC(rt->b,namax+1,MMG5_int,goto fail);
  MMG5_SAFE_CALLOC(rt->eta,namax+1,MMG5_int,goto fail);
  MMG5_SAFE_CALLOC(rt->etb,namax+1,MMG5_int,goto fail);
  MMG5_SAFE_CALLOC(rt->refa,3*(namax+1),double,goto fail);
  MMG5_SAFE_CALLOC(rt->refb,3*(namax+1),double,goto fail);
  rt->pos = NULL;

  for (j=0; j<rt->n; j++) {
    rt->a[j] = per->a[j+1];
    rt->b[j] = per->b[j+1];
    memcpy(&rt->refa[3*j],mesh->point[rt->a[j]].c,3*sizeof(double));
    memcpy(&rt->refb[3*j],mesh->point[rt->b[j]].c,3*sizeof(double));
  }

  /* normalized rigid motion:
   * x' = (x-min)/delta  =>  xb' = R xa' + (R min + t - min)/delta */
  dd = mesh->info.delta;
  if ( !(dd > 0.) ) goto fail;
  memcpy(rt->R,per->R,9*sizeof(double));
  per_apply(per->R,per->t,mesh->info.min,s);
  rt->t[0] = (s[0]-mesh->info.min[0])/dd;
  rt->t[1] = (s[1]-mesh->info.min[1])/dd;
  rt->t[2] = (s[2]-mesh->info.min[2])/dd;

  if ( !per_buildPos(mesh) ) goto fail;
  if ( !per_buildEdgeTria(mesh) ) goto fail;

  per->active = 1;
  return 1;

fail:
  if ( rt->a ) MMG5_SAFE_FREE(rt->a);
  if ( rt->b ) MMG5_SAFE_FREE(rt->b);
  if ( rt->eta ) MMG5_SAFE_FREE(rt->eta);
  if ( rt->etb ) MMG5_SAFE_FREE(rt->etb);
  if ( rt->pos ) MMG5_SAFE_FREE(rt->pos);
  if ( rt->refa ) MMG5_SAFE_FREE(rt->refa);
  if ( rt->refb ) MMG5_SAFE_FREE(rt->refb);
  MMG5_SAFE_FREE(rt);
  perRun = NULL;
  return 0;
}

/* Finish a run: write the compacted chains back to the persistent pair. */
void MMGS_periodic_end(MMG5_pMesh mesh) {
  MMG5_pPeriodicPair per;
  MMGS_PerRuntime   *rt;
  MMG5_int           j,namax;

  if ( !mesh->per ) return;
  per = mesh->per;
  per->active = 0;
  rt = perRun;
  if ( !rt ) return;

  /* replace the persistent arrays by the compacted runtime ones */
  if ( per->a ) MMG5_DEL_MEM(mesh,per->a);
  if ( per->b ) MMG5_DEL_MEM(mesh,per->b);
  namax = rt->n + 8;
  MMG5_ADD_MEM(mesh,2*(namax+1)*sizeof(MMG5_int),"periodic pair chains",
               goto cleanup);
  MMG5_SAFE_CALLOC(per->a,namax+1,MMG5_int,goto cleanup);
  MMG5_SAFE_CALLOC(per->b,namax+1,MMG5_int,goto cleanup);
  per->namax = namax;
  per->n = rt->n;
  for (j=0; j<rt->n; j++) {
    per->a[j+1] = rt->a[j];
    per->b[j+1] = rt->b[j];
  }

cleanup:
  if (rt->a) MMG5_SAFE_FREE(rt->a);
  if (rt->b) MMG5_SAFE_FREE(rt->b);
  if (rt->eta) MMG5_SAFE_FREE(rt->eta);
  if (rt->etb) MMG5_SAFE_FREE(rt->etb);
  if (rt->pos) MMG5_SAFE_FREE(rt->pos);
  if (rt->refa) MMG5_SAFE_FREE(rt->refa);
  if (rt->refb) MMG5_SAFE_FREE(rt->refb);
  MMG5_SAFE_FREE(rt);
  perRun = NULL;
}

/* -------------------------------------------------------------------------- */
/* Lookups                                                                     */
/* -------------------------------------------------------------------------- */

/* Return 1 and fill *side (0=A,1=B) and *j if (u,v) is a chain edge. */
static int per_chainEdge(MMG5_int u,MMG5_int v,int *side,MMG5_int *j) {
  MMGS_PerRuntime *rt = perRun;
  MMG5_int         k,nedge;

  nedge = per_nedge(rt);
  for (k=0; k<nedge; k++) {
    MMG5_int kn = per_next(rt,k);
    if ( (rt->a[k]==u && rt->a[kn]==v) || (rt->a[k]==v && rt->a[kn]==u) ) {
      *side = 0; *j = k; return 1;
    }
    if ( (rt->b[k]==u && rt->b[kn]==v) || (rt->b[k]==v && rt->b[kn]==u) ) {
      *side = 1; *j = k; return 1;
    }
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Reference polyline geometry                                                 */
/* -------------------------------------------------------------------------- */

/* Project x on segment [p0,p1], return the distance and the parameter s. */
static double per_projectSeg(const double *x,const double *p0,
                             const double *p1,double *sout) {
  double e[3],w[3],ll,s,y[3],d;
  int    i;

  for (i=0; i<3; i++) e[i] = p1[i]-p0[i];
  ll = e[0]*e[0]+e[1]*e[1]+e[2]*e[2];
  if ( ll < MMG5_EPSD2 ) {
    *sout = 0.;
    return per_dist3(x,p0);
  }
  s = 0.;
  for (i=0; i<3; i++) { w[i] = x[i]-p0[i]; s += w[i]*e[i]; }
  s /= ll;
  if ( s < 0. ) s = 0.;
  else if ( s > 1. ) s = 1.;
  d = 0.;
  for (i=0; i<3; i++) {
    y[i] = p0[i] + s*e[i];
    d += (x[i]-y[i])*(x[i]-y[i]);
  }
  *sout = s;
  return sqrt(d);
}

/* Check that x sits on reference segment (j0,j1) of one chain at parameter s. */
static int per_onReference(int side,MMG5_int j0,MMG5_int j1,double s,
                           const double *x) {
  MMGS_PerRuntime *rt = perRun;
  const double    *ref = side ? rt->refb : rt->refa;
  double           d,ss;

  if ( j1 <= j0 || j0 < 0 || j1 >= rt->n ) return 0;
  d = per_projectSeg(x,&ref[3*j0],&ref[3*j1],&ss);
  if ( d > MMGS_PER_PROJ_EPS ) return 0;
  if ( fabs(ss-s) > MMGS_PER_PROJ_EPS &&
       fabs(ss-s) > 1.0e-6*MG_MAX(1.,fabs(s)) ) return 0;
  return 1;
}

/* Check the rigid motion relation between two chain points. */
static int per_checkPair(const double *xa,const double *xb) {
  double y[3];
  per_apply(perRun->R,perRun->t,xa,y);
  return per_dist3(y,xb) <= MMGS_PER_MAP_EPS;
}

/* -------------------------------------------------------------------------- */
/* Shared size field                                                           */
/* -------------------------------------------------------------------------- */

void MMGS_periodic_applyCommonSizes(MMG5_pMesh mesh,MMG5_pSol met) {
  MMGS_PerRuntime *rt;
  MMG5_int         j;

  if ( !MMGS_periodic_active(mesh) || !met || !met->m ) return;
  rt = perRun;
  for (j=0; j<rt->n; j++) {
    double h = MG_MIN(met->m[rt->a[j]],met->m[rt->b[j]]);
    met->m[rt->a[j]] = h;
    met->m[rt->b[j]] = h;
  }
}

/* Common size interpolated by arc length on chain edge j at parameter s. */
static double per_edgeSize(MMG5_pSol met,MMG5_int j,double s) {
  MMG5_int jn = per_next(perRun,j);
  double   h0 = met->m[perRun->a[j]];
  double   h1 = met->m[perRun->a[jn]];
  return (1.0-s)*h0 + s*h1;
}

/* -------------------------------------------------------------------------- */
/* Runtime array growth                                                        */
/* -------------------------------------------------------------------------- */

static int per_runGrow(MMG5_pMesh mesh,MMG5_int want) {
  MMGS_PerRuntime *rt = perRun;
  MMG5_int         newmax;

  if ( want+1 <= rt->namax ) return 1;
  newmax = (MMG5_int)(1.3 * (double)want) + 16;

  MMG5_ADD_MEM(mesh,(newmax-rt->namax)*
               (4*sizeof(MMG5_int)+6*sizeof(double)),
               "periodic runtime arrays",return 0);

  MMG5_SAFE_REALLOC(rt->a,rt->namax+1,newmax+1,MMG5_int,
                    "periodic chain",return 0);
  MMG5_SAFE_REALLOC(rt->b,rt->namax+1,newmax+1,MMG5_int,
                    "periodic chain",return 0);
  MMG5_SAFE_REALLOC(rt->eta,rt->namax+1,newmax+1,MMG5_int,
                    "periodic edge tria",return 0);
  MMG5_SAFE_REALLOC(rt->etb,rt->namax+1,newmax+1,MMG5_int,
                    "periodic edge tria",return 0);
  MMG5_SAFE_REALLOC(rt->refa,3*(rt->namax+1),3*(newmax+1),double,
                    "periodic reference polyline",return 0);
  MMG5_SAFE_REALLOC(rt->refb,3*(rt->namax+1),3*(newmax+1),double,
                    "periodic reference polyline",return 0);
  rt->namax = newmax;
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Chain edge query                                                            */
/* -------------------------------------------------------------------------- */

int MMGS_periodic_isChainEdge(MMG5_pMesh mesh,MMG5_int u,MMG5_int v) {
  int      side;
  MMG5_int j;
  if ( !perRun ) return 0;
  return per_chainEdge(u,v,&side,&j);
}

/* -------------------------------------------------------------------------- */
/* Split                                                                       */
/* -------------------------------------------------------------------------- */

/*
 * Create a ridge/boundary point on edge (k,i) at coordinates c, mirroring the
 * ridge bookkeeping done by chkspl. Returns the new point index, 0 on failure.
 */
static MMG5_int per_newRidgePt(MMG5_pMesh mesh,MMG5_pSol met,
                               MMG5_int k,int8_t i,const double c[3]) {
  MMG5_pTria    pt = &mesh->tria[k];
  MMG5_pPoint   ppt;
  MMG5_int      ip;
  double        n[3],to[3],e[3],ll;
  int8_t        i1 = MMG5_inxt2[i];
  int8_t        i2 = MMG5_iprv2[i];
  int           cidx;

  /* tangent along the edge, normal of the incident triangle */
  for (cidx=0; cidx<3; cidx++)
    e[cidx] = mesh->point[pt->v[i2]].c[cidx]
            - mesh->point[pt->v[i1]].c[cidx];
  ll = sqrt(e[0]*e[0]+e[1]*e[1]+e[2]*e[2]);
  if ( ll > MMG5_EPSD2 ) {
    to[0]=e[0]/ll; to[1]=e[1]/ll; to[2]=e[2]/ll;
  } else {
    to[0]=1.; to[1]=to[2]=0.;
  }
  MMG5_nortri(mesh,pt,n);

  ip = MMGS_newPt(mesh,(double*)c, MG_EDG(pt->tag[i]) ? to : n);
  if ( !ip ) {
    MMGS_POINT_REALLOC(mesh,met,ip,mesh->gap,return 0,(double*)c,
                       MG_EDG(pt->tag[i]) ? to : n);
  }
  ppt = &mesh->point[ip];
  ppt->tag = pt->tag[i];
  if ( pt->edg[i] ) ppt->ref = pt->edg[i];

  if ( MG_EDG(pt->tag[i]) ) {
    ++mesh->xp;
    if ( mesh->xp > mesh->xpmax ) {
      MMG5_TAB_RECALLOC(mesh,mesh->xpoint,mesh->xpmax,MMG5_GAP,MMG5_xPoint,
                         "larger xpoint table",
                         MMGS_delPt(mesh,ip); return 0);
    }
    ppt->xp = mesh->xp;
    memcpy(mesh->xpoint[mesh->xp].n1,n,3*sizeof(double));
  }
  return ip;
}

/*
 * Paired edge split. When edge (k,i) belongs to one of the chains, this runs
 * the whole insertion on both sides: both candidate points are created and
 * both must pass the standard bulge/quality simulation before any topology
 * change is committed. A rejected candidate leaves the mesh untouched.
 *
 * \return 1 if the edge is a chain edge (the regular split is then skipped),
 * 0 if it is not a chain edge.
 */
int MMGS_periodic_trySplit(MMG5_pMesh mesh,MMG5_pSol met,
                           MMG5_int k,int8_t i) {
  MMGS_PerRuntime *rt = perRun;
  MMG5_pTria       pt;
  MMG5_int         u,v,ia,ib,ipA,ipB,packed,kp,j,newpos,jins;
  int               side,iter,okA,okB;
  int8_t            ipk;
  double            oA[3],mA[3],xA[3],xB[3],s,slo,shi,h;
  int               c;

  if ( !rt ) return 0;
  pt = &mesh->tria[k];
  u = pt->v[MMG5_inxt2[i]];
  v = pt->v[MMG5_iprv2[i]];
  if ( !per_chainEdge(u,v,&side,&j) ) return 0;

  /* partner edge */
  if ( side == 0 ) { ia = rt->b[j]; ib = rt->b[per_next(rt,j)]; }
  else             { ia = rt->a[j]; ib = rt->a[per_next(rt,j)]; }
  packed = per_findPackedEdge(mesh,ia,ib);
  if ( !packed ) return 1;
  kp  = packed / 3;
  ipk = (int8_t)(packed % 3);

  /* initial candidate: midpoint of each edge (mapped through the motion) */
  for (c=0; c<3; c++)
    mA[c] = 0.5*(mesh->point[u].c[c]+mesh->point[v].c[c]);
  per_apply(rt->R,rt->t,mA,xB);

  /* create both candidate points */
  ipA = per_newRidgePt(mesh,met,k,i,mA);
  if ( !ipA ) return 1;
  ipB = per_newRidgePt(mesh,met,kp,ipk,xB);
  if ( !ipB ) { MMGS_delPt(mesh,ipA); return 1; }

  /* initial common size, arc-length interpolation at s=0.5 */
  h = per_edgeSize(met,j,0.5);
  met->m[ipA] = h;
  met->m[ipB] = h;

  /* remember the bezier-style initial position for the dichotomy */
  memcpy(oA,mA,3*sizeof(double));

  /* joint simulation with a common arc parameter */
  okA = MMGS_simbulgept(mesh,met,k,i,ipA);
  okB = MMGS_simbulgept(mesh,met,kp,ipk,ipB);

  if ( !okA || !okB ) {
    slo = 0.; shi = 1.; s = 0.5;
    for (iter=0; iter<4; iter++) {
      s = 0.5*(slo+shi);
      for (c=0; c<3; c++) xA[c] = mA[c] + s*(oA[c]-mA[c]);
      per_apply(rt->R,rt->t,xA,xB);
      memcpy(mesh->point[ipA].c,xA,3*sizeof(double));
      memcpy(mesh->point[ipB].c,xB,3*sizeof(double));
      okA = MMGS_simbulgept(mesh,met,k,i,ipA);
      okB = MMGS_simbulgept(mesh,met,kp,ipk,ipB);
      if ( okA && okB ) { slo = s; }
      else             { shi = s; }
    }
    s = slo;
    for (c=0; c<3; c++) xA[c] = mA[c] + s*(oA[c]-mA[c]);
    per_apply(rt->R,rt->t,xA,xB);
    memcpy(mesh->point[ipA].c,xA,3*sizeof(double));
    memcpy(mesh->point[ipB].c,xB,3*sizeof(double));
    okA = MMGS_simbulgept(mesh,met,k,i,ipA);
    okB = MMGS_simbulgept(mesh,met,kp,ipk,ipB);
    if ( !okA || !okB ) {
      MMGS_delPt(mesh,ipA);
      MMGS_delPt(mesh,ipB);
      return 1;
    }
  }
  else s = 0.5;

  /* the candidates must lie on their reference segment */
  if ( side == 0 ) {
    if ( !per_onReference(0,j,j+1,s,mesh->point[ipA].c) ||
         !per_onReference(1,j,j+1,s,mesh->point[ipB].c) ) {
      MMGS_delPt(mesh,ipA); MMGS_delPt(mesh,ipB); return 1;
    }
  } else {
    if ( !per_onReference(1,j,j+1,s,mesh->point[ipA].c) ||
         !per_onReference(0,j,j+1,s,mesh->point[ipB].c) ) {
      MMGS_delPt(mesh,ipA); MMGS_delPt(mesh,ipB); return 1;
    }
  }

  /* grow the bookkeeping arrays before any topology commit so an allocation
   * failure cannot leave the pair inconsistent */
  newpos = rt->n+1;
  jins = j+1;
  if ( !per_runGrow(mesh,newpos) ) {
    MMGS_delPt(mesh,ipA); MMGS_delPt(mesh,ipB); return 1;
  }

  /* commit both splits */
  if ( !split1b(mesh,k,i,ipA) || !split1b(mesh,kp,ipk,ipB) ) {
    /* split1b only fails on allocation, which leaves the mesh consistent;
     * remove the unused candidates and give up this edge */
    if ( MG_VOK(&mesh->point[ipA]) ) MMGS_delPt(mesh,ipA);
    if ( MG_VOK(&mesh->point[ipB]) ) MMGS_delPt(mesh,ipB);
    return 1;
  }

  /* insert the new node pair at slot j+1 */
  for (j=rt->n; j>jins; j--) {
    rt->a[j] = rt->a[j-1];
    rt->b[j] = rt->b[j-1];
    memcpy(&rt->refa[3*j],&rt->refa[3*(j-1)],3*sizeof(double));
    memcpy(&rt->refb[3*j],&rt->refb[3*(j-1)],3*sizeof(double));
  }
  if ( side == 0 ) { rt->a[jins] = ipA; rt->b[jins] = ipB; }
  else             { rt->a[jins] = ipB; rt->b[jins] = ipA; }
  memcpy(&rt->refa[3*jins],mesh->point[rt->a[jins]].c,3*sizeof(double));
  memcpy(&rt->refb[3*jins],mesh->point[rt->b[jins]].c,3*sizeof(double));
  rt->n = newpos;

  if ( !per_buildEdgeTria(mesh) || !per_buildPos(mesh) ) return 1;

  /* final common size with the accepted arc parameter */
  h = per_edgeSize(met,j,s);
  met->m[ipA] = h;
  met->m[ipB] = h;

  if ( !per_checkPair(mesh->point[rt->a[jins]].c,
                      mesh->point[rt->b[jins]].c) ) return 1;
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Collapse                                                                    */
/* -------------------------------------------------------------------------- */

/* run the chkcol simulation and the matching colver variant on one edge */
static int per_commitCollapse(MMG5_pMesh mesh,MMG5_pSol met,
                              MMG5_int k,int8_t i) {
  MMG5_int list[MMG5_TRIA_LMAX+2];
  int      ilist,ier;
  int8_t   open;

  ilist = chkcol(mesh,met,k,i,list,2,MMG5_lenSurfEdg,MMG5_caltri_iso);
  if ( ilist <= 0 ) return 0;
  open = (mesh->adja[3*(k-1)+1+i] == 0);

  if ( ilist+open > 3 )
    ier = colver(mesh,list,ilist);
  else if ( ilist == 3 )
    ier = colver3(mesh,list);
  else {
    if ( !open ) return 0;
    ier = colver2(mesh,list);
  }
  return ier ? 1 : 0;
}

/*
 * Paired edge collapse. chkcol is run (read only) on both sides first and the
 * two removals must target the same chain slot; only then are both collapses
 * committed. Open-chain endpoints are preserved.
 *
 * \return 1 if the edge is a chain edge (the regular collapse is then
 * skipped), 0 if it is not a chain edge.
 */
int MMGS_periodic_tryCollapse(MMG5_pMesh mesh,MMG5_pSol met,
                              MMG5_int k,int8_t i) {
  MMGS_PerRuntime *rt = perRun;
  MMG5_pTria       pt;
  MMG5_int         u,v,ia,ib,packed,kp,j;
  MMG5_int         list[MMG5_TRIA_LMAX+2];
  int               side,dir,ilista,ilistb;
  int8_t            ipka,ipkb,openb;

  if ( !rt ) return 0;
  pt = &mesh->tria[k];
  u = pt->v[MMG5_inxt2[i]];
  v = pt->v[MMG5_iprv2[i]];
  if ( !per_chainEdge(u,v,&side,&j) ) return 0;

  /* partner edge endpoints in chain order (first node = slot j) */
  if ( side == 0 ) { ia = rt->b[j]; ib = rt->b[per_next(rt,j)]; }
  else             { ia = rt->a[j]; ib = rt->a[per_next(rt,j)]; }
  packed = per_findPackedEdge(mesh,ia,ib);
  if ( !packed ) return 1;
  kp = packed / 3;

  /*
   * The edge joins chain slots j and j+1. Try removing either slot. For each
   * candidate, select the local edge orientation on both triangles whose
   * inxt vertex is the removed node, simulate through chkcol and only commit
   * when both sides accept the same slot.
   */
  for (dir=0; dir<2; dir++) {
    MMG5_int slot = j + dir;             /* 0 -> slot j, 1 -> slot j+1 */
    MMG5_int remA,remB,keepA,keepB;
    int8_t   locA,locB;

    if ( side == 0 ) { remA = rt->a[slot]; remB = rt->b[slot]; }
    else             { remA = rt->b[slot]; remB = rt->a[slot]; }
    /* the survivor on each edge is its other chain node */
    keepA = (remA == u) ? v : u;
    keepB = (remB == ia) ? ib : ia;

    /* open chain endpoints cannot be removed */
    if ( !rt->closed && (slot == 0 || slot == rt->n-1) ) continue;

    locA = per_localEdgeRemoving(mesh,k,remA,keepA);
    locB = per_localEdgeRemoving(mesh,kp,remB,keepB);
    if ( locA < 0 || locB < 0 ) continue;

    /* read-only simulations on both sides: nothing is committed yet */
    ilista = chkcol(mesh,met,k,locA,list,2,MMG5_lenSurfEdg,MMG5_caltri_iso);
    if ( ilista <= 0 ) continue;
    ilistb = chkcol(mesh,met,kp,locB,list,2,MMG5_lenSurfEdg,MMG5_caltri_iso);
    if ( ilistb <= 0 ) continue;
    openb = (mesh->adja[3*(kp-1)+1+locB] == 0);
    if ( ilistb == 2 && !openb ) continue;

    /* commit both sides with the validated orientations */
    ipka = locA; ipkb = locB;
    if ( !per_commitCollapse(mesh,met,k,ipka) ) return 1;
    if ( !per_commitCollapse(mesh,met,kp,ipkb) ) return 1;

    /* rebuild the chains from the surviving node pairs */
    {
      MMG5_int nn = 0,idx;
      for (idx=0; idx<rt->n; idx++) {
        if ( MG_VOK(&mesh->point[rt->a[idx]]) &&
             MG_VOK(&mesh->point[rt->b[idx]]) ) {
          if ( nn != idx ) {
            rt->a[nn] = rt->a[idx];
            rt->b[nn] = rt->b[idx];
            memcpy(&rt->refa[3*nn],&rt->refa[3*idx],3*sizeof(double));
            memcpy(&rt->refb[3*nn],&rt->refb[3*idx],3*sizeof(double));
          }
          nn++;
        }
      }
      if ( nn != rt->n-1 ) return 1;
      rt->n = nn;
    }

    if ( !per_buildEdgeTria(mesh) || !per_buildPos(mesh) ) return 1;

    for (j=0; j<rt->n; j++) {
      if ( !per_checkPair(mesh->point[rt->a[j]].c,
                          mesh->point[rt->b[j]].c) ) return 1;
    }
    return 1;
  }

  /* no orientation is legal on both sides: reject without any modification */
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Tangential move                                                             */
/* -------------------------------------------------------------------------- */

/* check that every triangle of a ball keeps its orientation and quality */
static int per_ballValidList(MMG5_pMesh mesh,const MMG5_int *list,int ilist) {
  MMG5_pTria pt0;
  double     nold[3],nnew[3],ps;
  int        l;
  MMG5_int   k;

  for (l=0; l<ilist; l++) {
    k = list[l] / 3;
    if ( !MMG5_nortri(mesh,&mesh->tria[k],nold) ) return 0;
    pt0 = &mesh->tria[0];
    memcpy(pt0,&mesh->tria[k],sizeof(MMG5_Tria));
    if ( !MMG5_nortri(mesh,pt0,nnew) ) return 0;
    ps = nnew[0]*nold[0]+nnew[1]*nold[1]+nnew[2]*nold[2];
    if ( ps <= 0. ) return 0;
    if ( MMGS_ALPHAD*MMG5_caltri_iso(mesh,NULL,pt0) < MMGS_NULKAL ) return 0;
  }
  return 1;
}

/*
 * Ridge-point move wrapper around movridpt. The regular move is run on the
 * triggered side; the partner point follows through the rigid motion and both
 * balls are validated. A rejected candidate restores both points.
 */
int MMGS_periodic_movridpt(MMG5_pMesh mesh,MMG5_pSol met,
                           MMG5_int *list,int ilist) {
  MMGS_PerRuntime *rt = perRun;
  MMG5_pPoint      pa,pb;
  MMG5_int         p,ka,j,jp,jn;
  int               side,ier,c;
  int8_t            dummy;
  MMG5_int         lista[MMG5_TRIA_LMAX+2],listb[MMG5_TRIA_LMAX+2];
  int               ila,ilb;
  double            olda[3],oldb[3],oldna[3],oldnb[3],newb[3];
  double            oldg1[3],oldg2[3],oldh1[3],oldh2[3],sa,sb,da,db;

  if ( !rt ) return movridpt(mesh,met,list,ilist);

  p = mesh->tria[list[0]/3].v[list[0]%3];
  if ( !rt->pos[p] ) return movridpt(mesh,met,list,ilist);
  j = rt->pos[p]-1;

  /* open chain endpoints are fixed */
  if ( !rt->closed && (j == 0 || j == rt->n-1) ) return 0;

  side = (rt->a[j] == p) ? 0 : 1;
  if ( side == 0 ) { pa = &mesh->point[rt->a[j]]; pb = &mesh->point[rt->b[j]]; }
  else             { pb = &mesh->point[rt->a[j]]; pa = &mesh->point[rt->b[j]]; }

  memcpy(olda,pa->c,3*sizeof(double));
  memcpy(oldb,pb->c,3*sizeof(double));
  memcpy(oldna,pa->n,3*sizeof(double));
  memcpy(oldnb,pb->n,3*sizeof(double));
  if ( MG_EDG(pa->tag) ) {
    memcpy(oldg1,mesh->xpoint[pa->xp].n1,3*sizeof(double));
    memcpy(oldg2,mesh->xpoint[pa->xp].n2,3*sizeof(double));
  }
  if ( MG_EDG(pb->tag) ) {
    memcpy(oldh1,mesh->xpoint[pb->xp].n1,3*sizeof(double));
    memcpy(oldh2,mesh->xpoint[pb->xp].n2,3*sizeof(double));
  }

  /* regular ridge move on the triggered side */
  ier = movridpt(mesh,met,list,ilist);
  if ( !ier ) return 0;

  /* move the partner through the rigid motion */
  per_apply(rt->R,rt->t,pa->c,newb);
  memcpy(pb->c,newb,3*sizeof(double));
  for (c=0; c<3; c++)
    pb->n[c] = rt->R[c]*pa->n[0] + rt->R[3+c]*pa->n[1]
             + rt->R[6+c]*pa->n[2];
  if ( MG_EDG(pb->tag) ) {
    for (c=0; c<3; c++) {
      mesh->xpoint[pb->xp].n1[c] =
        rt->R[c]*mesh->xpoint[pa->xp].n1[0] +
        rt->R[3+c]*mesh->xpoint[pa->xp].n1[1] +
        rt->R[6+c]*mesh->xpoint[pa->xp].n1[2];
      mesh->xpoint[pb->xp].n2[c] =
        rt->R[c]*mesh->xpoint[pa->xp].n2[0] +
        rt->R[3+c]*mesh->xpoint[pa->xp].n2[1] +
        rt->R[6+c]*mesh->xpoint[pa->xp].n2[2];
    }
  }

  /* validate the two balls */
  ila = MMG5_boulet(mesh,list[0]/3,list[0]%3,lista,1,&dummy);
  /* the triangle incident to chain edge (j-1,j) contains the partner point */
  jp = per_prev(rt,j);
  ka = (side==0) ? rt->etb[jp] : rt->eta[jp];
  if ( !ka ) ka = (side==0) ? rt->etb[j] : rt->eta[j];
  if ( !ka ) return 0;
  {
    int8_t ipk = 0;
    MMG5_int kk;
    for (kk=0; kk<3; kk++)
      if ( mesh->tria[ka].v[kk] == pb-mesh->point ) { ipk = (int8_t)kk; break; }
    ilb = MMG5_boulet(mesh,ka,ipk,listb,1,&dummy);
  }
  if ( ila <= 0 || ilb <= 0 ||
       !per_ballValidList(mesh,lista,ila) ||
       !per_ballValidList(mesh,listb,ilb) ) {
    goto reject;
  }

  /* the points must stay on their reference segment and stay mapped */
  jp = per_prev(rt,j);
  jn = per_next(rt,j);
  da = per_projectSeg(pa->c,
       side==0 ? &rt->refa[3*jp] : &rt->refb[3*jp],
       side==0 ? &rt->refa[3*jn] : &rt->refb[3*jn],&sa);
  db = per_projectSeg(pb->c,
       side==0 ? &rt->refb[3*jp] : &rt->refa[3*jp],
       side==0 ? &rt->refb[3*jn] : &rt->refa[3*jn],&sb);
  if ( da > MMGS_PER_PROJ_EPS || db > MMGS_PER_PROJ_EPS ||
       fabs(sa-sb) > MMGS_PER_PROJ_EPS )
    goto reject;
  if ( !per_checkPair(pa->c,pb->c) ) goto reject;

  return 1;

reject:
  memcpy(pa->c,olda,3*sizeof(double));
  memcpy(pb->c,oldb,3*sizeof(double));
  memcpy(pa->n,oldna,3*sizeof(double));
  memcpy(pb->n,oldnb,3*sizeof(double));
  if ( MG_EDG(pa->tag) ) {
    memcpy(mesh->xpoint[pa->xp].n1,oldg1,3*sizeof(double));
    memcpy(mesh->xpoint[pa->xp].n2,oldg2,3*sizeof(double));
  }
  if ( MG_EDG(pb->tag) ) {
    memcpy(mesh->xpoint[pb->xp].n1,oldh1,3*sizeof(double));
    memcpy(mesh->xpoint[pb->xp].n2,oldh2,3*sizeof(double));
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Packing and reset                                                           */
/* -------------------------------------------------------------------------- */

/*
 * Re-index the pair after MMGS_packMesh compacts the point table. At that
 * point every surviving point carries its new index in point[].tmp.
 */
int MMGS_periodic_pack(MMG5_pMesh mesh) {
  MMG5_pPeriodicPair per;
  MMG5_int           j,nn;

  per = mesh->per;
  if ( !per ) return 1;

  nn = 0;
  for (j=1; j<=per->n; j++) {
    MMG5_int ia = per->a[j];
    MMG5_int ib = per->b[j];
    if ( !MG_VOK(&mesh->point[ia]) || !MG_VOK(&mesh->point[ib]) )
      continue;
    nn++;
    per->a[nn] = mesh->point[ia].tmp;
    per->b[nn] = mesh->point[ib].tmp;
  }
  per->n = nn;
  if ( per->n == 0 ) per_freePair(mesh);
  return 1;
}
