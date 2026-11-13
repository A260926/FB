#include "../ufbx.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static void require(int b,const char *s) { if(!b) {fprintf(stderr,"baseline: %s\n",s);exit(1);} }
static ufbx_vec3 world_pos(ufbx_mesh *m,size_t i) {
 ufbx_vec3 p=m->skinned_position.values.data[i];
 if(m->skinned_is_local && m->instances.count) p=ufbx_transform_position(&m->instances.data[0]->geometry_to_world,p);
 return p;
}
int main(void) {
 const char *files[]={"data/maya_anim_pivot_rotate_7700_ascii.fbx","data/blender_279_sausage_7400_binary.fbx","data/maya_blend_inbetween_7500_ascii.fbx"};
 double times[][2]={{0.01,0.09},{0.10,0.37},{0.15,0.65}};
 size_t n,f,total=0;
 for(f=0;f<3;f++) {
  ufbx_error err;ufbx_load_opts lo={0};lo.evaluate_skinning=true;
  ufbx_scene *s=ufbx_load_file(files[f],&lo,&err);
  if(!s) fprintf(stderr,"%s: %s\n",files[f],err.description.data);
  require(s!=NULL,"load");
  const ufbx_anim *anim=s->anim; if(s->anim_stacks.count) anim=s->anim_stacks.data[0]->anim;
  if(f==1) {ufbx_anim_stack *st=ufbx_find_anim_stack(s,"Skeleton|Wiggle");require(st!=NULL,"Wiggle stack");anim=st->anim;}
  printf("BASELINE_INPUT layers=%zu stacks=%zu selected_layers=%zu\n",s->anim_layers.count,s->anim_stacks.count,anim->layers.count);
  require(anim->layers.count==1,"single animation layer");
  ufbx_evaluate_opts eo={0};eo.evaluate_skinning=true;
  ufbx_scene *a=ufbx_evaluate_scene(s,anim,times[f][0],&eo,&err);
  ufbx_scene *b=ufbx_evaluate_scene(s,anim,times[f][1],&eo,&err);
  require(a&&b,"evaluate");double sum=0.0;size_t vertices=0,norm=0,adjust=0;
  for(n=0;n<s->nodes.count;n++) {norm+=s->nodes.data[n]->inherit_mode!=UFBX_INHERIT_MODE_NORMAL;adjust+=s->nodes.data[n]->has_adjust_transform;}
  require(norm==0&&adjust==0,"supported node transforms");
  for(n=0;n<a->meshes.count;n++) {
   ufbx_mesh *ma=a->meshes.data[n],*mb=b->meshes.data[n];
   require(ma->num_vertices==mb->num_vertices,"vertex identity");
   for(size_t i=0;i<ma->num_vertices;i++) {ufbx_vec3 p=world_pos(ma,i),q=world_pos(mb,i);
    require(isfinite(p.x)&&isfinite(p.y)&&isfinite(p.z)&&isfinite(q.x)&&isfinite(q.y)&&isfinite(q.z),"finite");
    sum+=fabs(p.x-q.x)+fabs(p.y-q.y)+fabs(p.z-q.z);vertices++;
   }
  }
  require(vertices>0&&sum>0.001,"actual animated geometry");total+=vertices;
  printf("BASELINE_SCENE file=%s vertices=%zu displacement_l1=%.9f nonnormal=%zu adjusted=%zu\n",files[f],vertices,sum,norm,adjust);
  ufbx_free_scene(a);ufbx_free_scene(b);ufbx_free_scene(s);
 }
 printf("UFBX_MOTION_BASELINE_OK scenes=3 vertices=%zu\n",total);return 0;
}
