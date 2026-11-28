// Fixed API consumer: minimal real loaded scene, extract, then native evaluation.
#include "ufbx.h"
#include <math.h>
#include <stdio.h>
int main(void){
 ufbx_error err;ufbx_scene *src=ufbx_load_file("task_support/tiny.fbx",NULL,&err);if(!src){fputs("PROBE_LOAD_FAIL\n",stderr);return 20;}
 uint32_t root=src->root_node->typed_id;ufbx_extract_opts opts={0};ufbx_extract_map map={0};
 ufbx_scene *s=ufbx_extract_scene(src,&root,1,&opts,&map,&err);if(!s){ufbx_free_scene(src);fputs("EXTRACT_API_FAILED\n",stderr);return 31;}
 if(s==src||s->nodes.count!=2||s->meshes.count!=1||map.node_ids.count!=2||map.element_ids.count!=src->elements.count){fputs("EXTRACT_IDENTITY_FAILED\n",stderr);return 32;}
 ufbx_free_scene(src);ufbx_scene *v=ufbx_evaluate_scene(s,NULL,.25,NULL,&err);if(!v)return 33;
 ufbx_node *n=ufbx_find_node(v,"Triangle");if(!n||!n->mesh||fabs(n->node_to_world.m03-2)>1e-8||n->mesh->num_vertices!=3)return 34;
 ufbx_free_scene(v);ufbx_free_scene(s);puts("EXTRACT_API_NATIVE_SCENE_OK");return 0;
}
