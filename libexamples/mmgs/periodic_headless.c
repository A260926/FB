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
 * \file libexamples/mmgs/periodic_headless.c
 * \brief Headless example of periodic boundary pair remeshing.
 *
 * Builds a flat triangulated rectangular strip whose two short edges are
 * periodic copies of one another (translation by one strip width), registers
 * them as a periodic pair, remeshes with a scalar target edge length and
 * writes the result to the mesh path given on the command line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "mmg/mmgs/libmmgs.h"

#define NX 6
#define NY 8

int main(int argc,char *argv[]) {
  MMG5_pMesh mesh = NULL;
  MMG5_pSol  met  = NULL;
  MMG5_int   np,nt,na[NY+1],nb[NY+1],nBefore,nAfter,cap;
  double     R[9],t[3];
  int        k,ix,iy,tid,ier;

  if ( argc != 2 ) {
    fprintf(stderr,"usage: %s output.mesh\n",argv[0]);
    return 1;
  }
  setvbuf(stdout,NULL,_IONBF,0);

  np = (NX+1)*(NY+1);
  nt = 2*NX*NY;

  if ( !MMGS_Init_mesh(MMG5_ARG_start,
                       MMG5_ARG_ppMesh,&mesh,
                       MMG5_ARG_ppMet,&met,
                       MMG5_ARG_end) )
    return 1;

  if ( !MMGS_Set_meshSize(mesh,np,nt,0) ) return 1;

  /* vertices of a unit-width, two-long flat grid in the z=0 plane */
  for (ix=0; ix<=NX; ix++) {
    for (iy=0; iy<=NY; iy++) {
      MMG5_int id = (MMG5_int)ix*(NY+1) + iy + 1;
      double x = (double)ix/(double)NX;
      double y = 2.0*(double)iy/(double)NY;
      if ( !MMGS_Set_vertex(mesh,x,y,0.0,0,id) ) return 1;
    }
  }

  /* two consistently oriented triangles per grid cell */
  tid = 0;
  for (ix=0; ix<NX; ix++) {
    for (iy=0; iy<NY; iy++) {
      MMG5_int p00 = (MMG5_int)ix*(NY+1)+iy+1;
      MMG5_int p10 = (MMG5_int)(ix+1)*(NY+1)+iy+1;
      MMG5_int p11 = (MMG5_int)(ix+1)*(NY+1)+(iy+1)+1;
      MMG5_int p01 = (MMG5_int)ix*(NY+1)+(iy+1)+1;
      if ( !MMGS_Set_triangle(mesh,p00,p10,p11,1,++tid) ) return 1;
      if ( !MMGS_Set_triangle(mesh,p00,p11,p01,1,++tid) ) return 1;
    }
  }

  /* periodic chains: the two short sides of the strip, ordered by y */
  for (iy=0; iy<=NY; iy++) {
    na[iy] = 1 + iy;               /* x = 0 */
    nb[iy] = NX*(NY+1) + 1 + iy;   /* x = 1 */
  }

  /* identity rotation, translation of one strip width along x */
  for (k=0; k<9; k++) R[k] = 0.;
  R[0] = R[4] = R[8] = 1.;
  t[0] = 1.0; t[1] = t[2] = 0.;

  if ( !MMGS_Set_periodicPair(mesh,NY+1,na,nb,R,t,0) ) {
    fprintf(stderr,"periodic pair registration failed\n");
    return 1;
  }

  /* scalar target edge length, smaller on the first chain's neighborhood */
  if ( !MMGS_Set_solSize(mesh,met,MMG5_Vertex,np,MMG5_Scalar) ) return 1;
  for (ix=0; ix<=NX; ix++) {
    for (iy=0; iy<=NY; iy++) {
      MMG5_int id = (MMG5_int)ix*(NY+1) + iy + 1;
      double h = 0.18 + 0.20*(double)ix/(double)NX;
      if ( !MMGS_Set_scalarSol(met,h,id) ) return 1;
    }
  }

  if ( !MMGS_Chk_meshData(mesh,met) ) return 1;

  cap = 0;
  /* capacity query: the call is refused but still reports the needed length */
  if ( MMGS_Get_periodicPair(mesh,0,NULL,NULL,&cap) != 0 || cap != NY+1 ) {
    fprintf(stderr,"Get capacity query failed\n");
    return 1;
  }
  nBefore = cap;
  printf("periodic pair points before remeshing: %" MMG5_PRId "\n",nBefore);

  if ( !MMGS_Set_iparameter(mesh,met,MMGS_IPARAM_verbose,-1) ) return 1;

  ier = MMGS_mmgslib(mesh,met);
  if ( ier != MMG5_SUCCESS ) {
    fprintf(stderr,"MMGS_mmgslib failed: %d\n",ier);
    return 1;
  }

  if ( !MMGS_Get_periodicPair(mesh,1<<20,na,nb,&nAfter) ) {
    fprintf(stderr,"Get after remeshing failed\n");
    return 1;
  }
  printf("periodic pair points after remeshing:  %" MMG5_PRId "\n",nAfter);

  /* check the rigid motion relation on every compacted pair point */
  for (k=0; k<nAfter; k++) {
    double ca[3],cb[3],d;
    MMG5_int ref;
    int corner,required;
    if ( !MMGS_GetByIdx_vertex(mesh,&ca[0],&ca[1],&ca[2],&ref,&corner,&required,na[k]) )
      return 1;
    if ( !MMGS_GetByIdx_vertex(mesh,&cb[0],&cb[1],&cb[2],&ref,&corner,&required,nb[k]) )
      return 1;
    d = fabs((ca[0]+1.0)-cb[0]) + fabs(ca[1]-cb[1]) + fabs(ca[2]-cb[2]);
    if ( d > 1.0e-8 ) {
      fprintf(stderr,"pair relation broken at node %d (err=%g)\n",k,d);
      return 1;
    }
  }

  MMGS_Get_meshSize(mesh,&np,&nt,NULL);
  printf("remeshed mesh: %" MMG5_PRId " vertices, %" MMG5_PRId " triangles\n",
         np,nt);

  if ( !MMGS_saveMesh(mesh,argv[1]) ) {
    fprintf(stderr,"unable to save %s\n",argv[1]);
    return 1;
  }
  printf("mesh written to %s\n",argv[1]);

  MMGS_Free_all(MMG5_ARG_start,
                MMG5_ARG_ppMesh,&mesh,
                MMG5_ARG_ppMet,&met,
                MMG5_ARG_end);
  return 0;
}
