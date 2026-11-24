#include "cgltf.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){
 cgltf_options o={0}; cgltf_data*d=0; const char*p=argc>1?argv[1]:"task_support/normal.gltf";
 cgltf_result r=cgltf_parse_file(&o,p,&d); if(r){fprintf(stderr,"parse %d\n",r);return 1;}
 r=cgltf_load_buffers(&o,d,p);if(!r)r=cgltf_validate(d);if(r){cgltf_free(d);return 2;}
 size_t values=0;for(size_t a=0;a<d->accessors_count;a++){
  cgltf_accessor*x=&d->accessors[a];for(size_t i=0;i<x->count;i++){float v[16]={0};
   if(!cgltf_accessor_read_float(x,i,v,cgltf_num_components(x->type))){cgltf_free(d);return 3;}
   for(size_t k=0;k<cgltf_num_components(x->type);k++){if(!isfinite(v[k])){cgltf_free(d);return 4;}values++;}
  }
 }
 int witness=0;for(size_t m=0;m<d->meshes_count;m++)for(size_t p0=0;p0<d->meshes[m].primitives_count;p0++){
  cgltf_primitive*p=&d->meshes[m].primitives[p0];for(size_t k=0;k<p->attributes_count;k++){
   cgltf_attribute*x=&p->attributes[k];if(x->type!=cgltf_attribute_type_position)continue;
   cgltf_accessor*a=x->data;if(!a->buffer_view||a->offset==0||a->stride<=12)continue;
   size_t uses=0;for(size_t mm=0;mm<d->meshes_count;mm++)for(size_t pp=0;pp<d->meshes[mm].primitives_count;pp++)
    for(size_t kk=0;kk<d->meshes[mm].primitives[pp].attributes_count;kk++)
     if(d->meshes[mm].primitives[pp].attributes[kk].data==a)uses++;
   if(uses>=2)witness=1;
  }
 }
 if(!witness||!values){cgltf_free(d);return 5;}
 if(argc==1){float v[3];if(d->meshes_count!=1||d->accessors_count!=2||!cgltf_accessor_read_float(&d->accessors[0],1,v,3)||v[0]!=1||v[1]!=0||v[2]!=0){cgltf_free(d);return 6;}}
 printf("CGLTF_NORMAL_OK accessors=%zu values=%zu shared_strided=%d\n",(size_t)d->accessors_count,values,witness);cgltf_free(d);return 0;
}
