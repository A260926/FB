#define PAR_OCTASPHERE_IMPLEMENTATION
#include "par_octasphere.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
 par_octasphere_config c={.corner_radius=.4f,.width=2,.height=3,.depth=4,.num_subdivisions=2};
 uint32_t ni,nv;par_octasphere_get_counts(&c,&ni,&nv);
 par_octasphere_mesh m={0};
 m.positions=malloc(3*nv*sizeof(float));m.normals=malloc(3*nv*sizeof(float));
 m.indices=malloc(ni*sizeof(uint16_t));par_octasphere_populate(&c,&m);
 assert(m.num_vertices==nv && m.num_indices<=ni);
 const double a[]={.6,1.1,1.6};
 for(uint32_t i=0;i<nv;i++) {
  double sum=0,norm=0;
  for(int k=0;k<3;k++) {
   double q=fmax(fabs(m.positions[3*i+k])-a[k],0);
   sum+=q*q;norm+=m.normals[3*i+k]*m.normals[3*i+k];
  }
  assert(fabs(sqrt(sum)-.4)<2e-6);assert(fabs(norm-1)<2e-6);
 }
 for(uint32_t i=0;i<m.num_indices;i++)assert(m.indices[i]<nv);
 printf("BASELINE rounded-box surface PASS vertices=%u indices=%u\n",m.num_vertices,m.num_indices);
 free(m.positions);free(m.normals);free(m.indices);
}
