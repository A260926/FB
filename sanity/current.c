#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../renderer/shaders/blinn_shader.h"
#include "../renderer/shaders/pbr_shader.h"
int main(void) {
 blinn_uniforms_t u; blinn_varyings_t v; int discard=0,i;
 texture_t *map=texture_create(3,3); vec4_t lit,dark;
 memset(&u,0,sizeof u); memset(&v,0,sizeof v);
 u.basecolor=vec4_new(1,1,1,1); u.light_dir=vec3_new(0,0,-1);
 u.camera_pos=vec3_new(0,0,2); u.punctual_intensity=1; u.shadow_map=map;
 v.normal=vec3_new(0,0,1); v.depth_position=vec3_new(0,0,0);
 for(i=0;i<9;i++)map->buffer[i]=vec4_new(1,1,1,1);
 lit=blinn_fragment_shader(&v,&u,&discard,0);
 for(i=0;i<9;i++)map->buffer[i]=vec4_new(0,0,0,1);
 dark=blinn_fragment_shader(&v,&u,&discard,0);
 assert(!discard && lit.x>0.99f && dark.x==0);
 texture_release(map); puts("HARD_SHADOW_BASELINE_OK lit=1 dark=0"); return 0;
}
