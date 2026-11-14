#include <stdint.h>
#include <bvh/v2/c_api/bvh.h>
#include <math.h>
#include <stdio.h>
struct data {struct bvh3f* tree;size_t id;float t;};
static bool callback(void* user,float* tmax,size_t begin,size_t end){
 struct data* d=user;bool hit=false;
 for(size_t i=begin;i<end;++i){size_t id=bvh3f_get_prim_id(d->tree,i);float t=(id==0?2.f:4.f)/2.f;
  if(t<=*tmax){*tmax=t;d->id=id;d->t=t;hit=true;}
 }return hit;
}
static int run(struct bvh3f* tree){
 struct data d={tree,SIZE_MAX,0};struct bvh_ray3f ray={{0,0,0},{0,0,2},0,10};
 struct bvh_intersect_callbackf cb={&d,callback};bvh3f_intersect_ray_robust(tree,&ray,&cb);
 if(d.id!=0||fabsf(d.t-1)>1e-6f)return 1;return 0;
}
int main(void){
 struct bvh_bbox3f boxes[]={{{-1,-1,2},{1,1,2}},{{-1,-1,4},{1,1,4}}};
 struct bvh_vec3f centers[]={{0,0,2},{0,0,4}};
 struct bvh_build_config cfg={BVH_BUILD_QUALITY_LOW,1,1,1024};
 struct bvh3f* tree=bvh3f_build(NULL,boxes,centers,2,&cfg);if(!tree||run(tree))return 1;
 FILE* f=tmpfile();if(!f)return 2;bvh3f_save(tree,f);rewind(f);struct bvh3f* restored=bvh3f_load(f);fclose(f);
 if(!restored||run(restored)||bvh3f_get_prim_count(restored)!=2)return 3;
 f=fopen("checks/legacy-static.bvh","rb");if(!f)return 4;
 struct bvh3f* legacy=bvh3f_load(f);fclose(f);
 if(!legacy||run(legacy)||bvh3f_get_prim_count(legacy)!=2)return 5;
 bvh3f_destroy(legacy);
 printf("BASELINE_C_ABI_OK id=0 t=1 primitives=%zu\n",bvh3f_get_prim_count(restored));
 bvh3f_destroy(restored);bvh3f_destroy(tree);return 0;
}
