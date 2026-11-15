#include "anim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"normal failed line=%d\n",__LINE__);exit(1);}}while(0)
static int near(float a,float b){return fabsf(a-b)<1e-5f;}
int main(void){
 struct anm_node root,child;float m[16],inv[16],point[3]={1,2,3},out[3];int i,j;
 REQUIRE(anm_init_node(&root)==0);REQUIRE(anm_init_node(&child)==0);
 REQUIRE(anm_add_node_animation(&root)==0);REQUIRE(anm_add_node_animation(&child)==0);
 REQUIRE(anm_use_node_animation(&root,1)==0);REQUIRE(anm_use_node_animation(&root,0)==0);
 REQUIRE(anm_use_node_animation(&child,1)==0);REQUIRE(anm_use_node_animation(&child,0)==0);
 anm_link_node(&root,&child);
 anm_set_position3f(&root,2,0,0,0);anm_set_position3f(&root,6,0,0,1000);
 anm_set_position3f(&child,0,3,0,0);
 anm_get_matrix(&child,m,500);anm_get_inv_matrix(&child,inv,500);
 REQUIRE(near(m[12],4)&&near(m[13],3));
 for(i=0;i<4;i++)for(j=0;j<4;j++){
  float v=0;int k;for(k=0;k<4;k++)v+=m[k*4+i]*inv[j*4+k];
  REQUIRE(near(v,i==j?1.f:0.f));
 }
 for(i=0;i<3;i++)out[i]=m[i]*point[0]+m[4+i]*point[1]+m[8+i]*point[2]+m[12+i];
 REQUIRE(near(out[0],5)&&near(out[1],5)&&near(out[2],3));
 anm_set_position3f(&root,6,0,0,0);anm_get_matrix(&child,m,500);
 REQUIRE(near(m[12],6)&&near(m[13],3));
 anm_destroy_node(&child);anm_destroy_node(&root);
#ifdef ANIM_THREAD_SAFE
 puts("LIBANIM_NORMAL_OK mode=thread world=6,3 vertex=5,5,3");
#else
 puts("LIBANIM_NORMAL_OK mode=plain world=6,3 vertex=5,5,3");
#endif
 return 0;
}
