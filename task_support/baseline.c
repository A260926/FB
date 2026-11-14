#include "headless.h"
int main(void){
 volume_t *v=volume_new();
 int p[3]={4,4,4}, origin[3]={0,0,0},size,sub;
 uint8_t rgba[4]={240,30,50,64};
 voxel_vertex_t *out=calloc(16*16*16*6*4,sizeof(*out));
 volume_set_at(v,NULL,p,rgba);
 int low=volume_generate_vertices(v,origin,0,out,&size,&sub);
 if(low!=0)return 1;
 rgba[3]=255;volume_set_at(v,NULL,p,rgba);
 int high=volume_generate_vertices(v,origin,0,out,&size,&sub);
 if(high!=6||size!=4||sub!=1)return 2;
 for(int i=0;i<high*size;++i)if(out[i].color[3]!=255)return 3;
 volume_t *copy=volume_copy(v);
 volume_clear(v);uint8_t got[4];volume_get_at(copy,NULL,p,got);
 if(got[3]!=255)return 4;
 int triangles=volume_generate_vertices(copy,origin,EFFECT_MARCHING_CUBES|EFFECT_MC_SMOOTH,out,&size,&sub);
 if(triangles<=0||size!=3)return 5;
 volume_mesh_t *mesh=volume_generate_mesh(copy,0,NULL,0);
 if(mesh->indices_count!=36||mesh->vertices_count==0)return 6;
 printf("GOXEL_BASELINE_OK alpha64_faces=%d alpha255_faces=%d mc_triangles=%d export_indices=%d cow_alpha=%u\n",low,high,triangles,mesh->indices_count,got[3]);
 volume_mesh_free(mesh);volume_delete(v);volume_delete(copy);free(out);
 return 0;
}
