#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <common/mempool.h>
#include <common/hashtable.h>
#include <datatypes/scene.h>
#include <nodes/bsdfnode.h>
#include <nodes/shaders/add.h>
#include <nodes/shaders/diffuse.h>
#include <nodes/shaders/emission.h>
#include <nodes/textures/constant.h>
#include <nodes/valuenode.h>
#include <renderer/samplers/sampler.h>
#include <renderer/pathtrace.h>
#include <accelerators/bvh.h>
#include <nodes/shaders/background.h>
int main(void){
 struct node_storage s={0};s.node_pool=newBlock(NULL,1024);s.node_table=newHashtable(compareNodes,&s.node_pool);
 sampler *rng=sampler_new();sampler_init(rng,Random,0,1,42);
 const struct bsdfNode *a=newDiffuse(&s,newConstantTexture(&s,(struct color){.red=.2f,.green=.3f,.blue=.4f,.alpha=1}));
 const struct bsdfNode *aa=newAdd(&s,a,a);
 struct lightRay incident={.start={0,1,0},.direction={0,-1,0}};
 struct hitRecord hit={.incident=&incident,.hitPoint={0,0,0},.surfaceNormal={0,1,0}};
 const struct bsdfSample normal=a->sample(a,rng,&hit), doubled=aa->sample(aa,rng,&hit);
 printf("DIFFUSE weight=%.3f %.3f %.3f\nADD_SAME alias=%d weight=%.3f %.3f %.3f pdf=%.3f\n",normal.weight.red,normal.weight.green,normal.weight.blue,a==aa,doubled.weight.red,doubled.weight.green,doubled.weight.blue,doubled.pdf);
 if(fabsf(normal.weight.red-.2f)>1e-5f||!isfinite(normal.out.direction.y)||normal.out.direction.y<0)return 3;
 const struct bsdfNode *e=newEmission(&s,newConstantTexture(&s,(struct color){.red=1,.green=0,.blue=0,.alpha=1}),newConstantValue(&s,2));
 const struct bsdfNode *sum=newAdd(&s,e,a);struct bsdfSample er=sum->sample(sum,rng,&hit);
 printf("ADD_EMISSION observed_red=%.3f\n",er.emitted.red);
 struct world empty={0};empty.topLevel=build_top_level_bvh(NULL);empty.background=newBackground(&s,newConstantTexture(&s,(struct color){.red=.25f,.green=.5f,.blue=.75f,.alpha=1}),newConstantValue(&s,1),NULL,false);
 struct color escaped=path_trace(incident,&empty,0,rng);
 printf("PATH_TRACE background=%.3f %.3f %.3f\n",escaped.red,escaped.green,escaped.blue);
 if(fabsf(escaped.red-.25f)>1e-5f||fabsf(escaped.green-.5f)>1e-5f||fabsf(escaped.blue-.75f)>1e-5f)return 4;
 destroy_bvh(empty.topLevel);
 puts("BASELINE_OK real diffuse and add graph sampled");
 sampler_destroy(rng);destroyHashtable(s.node_table);destroyBlocks(s.node_pool);return 0;
}
