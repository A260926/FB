#include "ufbx.h"
#include <stdio.h>
#include <math.h>
static int one(const char *path){
 ufbx_error error;ufbx_load_opts load={0};ufbx_scene *src=ufbx_load_file(path,&load,&error);if(!src){fprintf(stderr,"load failed: %s\n",path);return 10;}
 if(!src->meshes.count||!src->skin_clusters.count){ufbx_free_scene(src);return 11;}
 double sum=0;size_t observed=0;
 const double times[]={0,4.0/24.0,8.0/24.0};
 for(size_t t=0;t<3;t++){
  ufbx_evaluate_opts opts={0};opts.evaluate_skinning=true;ufbx_scene *s=ufbx_evaluate_scene(src,NULL,times[t],&opts,&error);if(!s){ufbx_free_scene(src);return 12;}
  for(size_t m=0;m<s->meshes.count;m++){
   ufbx_mesh *mesh=s->meshes.data[m];if(!mesh->skinned_position.exists){ufbx_free_scene(s);ufbx_free_scene(src);return 13;}
   for(size_t i=0;i<mesh->num_indices;i++){ufbx_vec3 p=ufbx_get_vertex_vec3(&mesh->skinned_position,i);if(!isfinite(p.x)||!isfinite(p.y)||!isfinite(p.z))return 14;sum+=p.x+2*p.y+3*p.z;observed++;}
  }
  ufbx_free_scene(s);
 }
 printf("UFBX_SKIN_SCENE_OK file=%s nodes=%zu clusters=%zu observations=%zu checksum=%.9f\n",path,src->nodes.count,src->skin_clusters.count,observed,sum);ufbx_free_scene(src);return observed?0:15;
}
int main(){int e=one("data/maya_instanced_skin_7700_ascii.fbx");if(e)return e;return one("data/maya_advanced_skinned_pivot_7700_ascii.fbx");}
